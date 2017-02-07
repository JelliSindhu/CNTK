//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"
#include "CNTKLibrary.h"
#include "Utils.h"
#include "Learner.h"
#ifndef CPUONLY
#include "cuda_runtime.h"
#define CUDA_CALL(expr) do {if (cudaSuccess != expr) LogicError("CUDA_FAILURE: "#expr);} while(0)
#endif
namespace
{
    const std::wstring learnersPropertyName = L"Learners";
    const std::wstring externalStatePropertyName = L"ExternalState";
}

namespace CNTK
{
#ifndef CPUONLY
    struct PinnedScalars
    {
        double m_trainingLoss;
        double m_evalCriterion;
    };

    class TrainerAsyncScalarData
    {
        PinnedScalars* m_pinnedScalars;
        cudaEvent_t m_scalarDoneEvent;

    public:
        TrainerAsyncScalarData()
        {
            cudaMallocHost(&m_pinnedScalars, sizeof(PinnedScalars));
            cudaEventCreate(&m_scalarDoneEvent);
        }

        ~TrainerAsyncScalarData()
        {
            cudaFreeHost(m_pinnedScalars);
            cudaEventDestroy(m_scalarDoneEvent);
        }

        static PinnedScalars* GetPinnedScalars(void* p)
        {
            return ((TrainerAsyncScalarData*)p)->m_pinnedScalars;
        }

        static cudaEvent_t GetScalarDoneEvent(void* p)
        {
            return ((TrainerAsyncScalarData*)p)->m_scalarDoneEvent;
        }
    };
#endif

    Trainer::Trainer(const FunctionPtr& model, const FunctionPtr& lossFunction, const std::vector<LearnerPtr>& parameterLearners)
        : Trainer(model, lossFunction, nullptr, parameterLearners)
    {}

    Trainer::Trainer(const FunctionPtr& model, const FunctionPtr& lossFunction, const FunctionPtr& evaluationFunction, const std::vector<LearnerPtr>& parameterLearners)
        : m_model(model),
          m_lossFunction(lossFunction),
          m_evaluationFunction(evaluationFunction),
          m_parameterLearners(std::make_shared<Learners>(parameterLearners)),
          m_prevMinibatchNumSamples(1),
          m_distributed(false),
          m_pAsyncScalarData(nullptr)
    {
        // By default we set the number of threads to hardware concurrency.
        if (!Internal::MaxNumCPUThreadsSet())
            SetMaxNumCPUThreads(std::thread::hardware_concurrency());

        std::vector<Variable> combinedFunctionArgs = { m_model, m_lossFunction };
        if (!m_lossFunction->Output().DynamicAxes().empty())
        {
            m_aggregatedLossFunction = ReduceSum(lossFunction);
            combinedFunctionArgs.push_back(m_aggregatedLossFunction);
            m_trainingSampleCountVar = m_lossFunction;
        }
        else
        {
            m_aggregatedLossFunction = m_lossFunction;
            m_trainingSampleCountVar = m_lossFunction->RootFunction()->Inputs()[0];
            if (model->Output() != m_trainingSampleCountVar)
                combinedFunctionArgs.push_back(m_trainingSampleCountVar);
        }

        if (m_evaluationFunction)
        {
            combinedFunctionArgs.push_back(m_evaluationFunction);

            if (!m_evaluationFunction->Output().DynamicAxes().empty())
            {
                m_aggregatedEvaluationFunction = ReduceSum(m_evaluationFunction);
                combinedFunctionArgs.push_back(m_aggregatedEvaluationFunction);
                m_testSampleCountVar = m_evaluationFunction;
            }
            else
            {
                m_aggregatedEvaluationFunction = m_evaluationFunction;
                m_testSampleCountVar = m_evaluationFunction->RootFunction()->Inputs()[0];
                if ((m_testSampleCountVar != m_trainingSampleCountVar) && (model->Output() != m_testSampleCountVar))
                    combinedFunctionArgs.push_back(m_testSampleCountVar);
            }
        }

        m_combinedTrainingFunction = Combine(combinedFunctionArgs);

        auto modelParameters = m_combinedTrainingFunction->Parameters();
        m_learnerParameters = m_parameterLearners->GetParameters();
        std::unordered_set<Parameter> modelParametersSet(modelParameters.begin(), modelParameters.end());
        std::unordered_set<Parameter> learnerParametersNotPartOfModel;
        for (const auto& learnerParameter : m_learnerParameters)
        {
            if (modelParametersSet.find(learnerParameter) == modelParametersSet.end())
                learnerParametersNotPartOfModel.insert(learnerParameter);
        }

        for (const auto& modelParameter : modelParametersSet)
        {
            if (m_learnerParameters.find(modelParameter) == m_learnerParameters.end())
                m_modelParametersNotCoveredByLearners.insert(modelParameter);
        }

        if (!learnerParametersNotPartOfModel.empty())
            InvalidArgument("Trainer ctor: %d of the learner parameters are not part of the model specified", (int)learnerParametersNotPartOfModel.size());

        if (!m_modelParametersNotCoveredByLearners.empty())
            fprintf(stderr, "[Note:] Trainer ctor: %d of the model parameters are not covered by any of the specified Learners; these parameters will not be learned\n", (int)m_modelParametersNotCoveredByLearners.size());

        m_distributed = m_parameterLearners->IsDistributed();
    }

    Trainer::~Trainer()
    {
#ifndef CPUONLY
        if (m_pAsyncScalarData)
        {
            delete (TrainerAsyncScalarData*)m_pAsyncScalarData;
        }
#endif
    }

    static double GetScalarValue(const ValuePtr& value)
    {
        if (value->Mask())
            LogicError("Scalar Value object cannot have an associated mask");

        auto scalarData = value->Data();
        if (scalarData->Shape().TotalSize() != 1)
            LogicError("Scalar Value object's has a size > 1");

        double scalar = std::numeric_limits<double>::quiet_NaN();
        NDArrayViewPtr cpuData;
        if (scalarData->Device() == DeviceDescriptor::CPUDevice())
            cpuData = scalarData;
        else
        {
            cpuData = std::make_shared<NDArrayView>(scalarData->GetDataType(), scalarData->Shape(), CNTK::DeviceDescriptor::CPUDevice());
            cpuData->CopyFrom(*scalarData);
        }

        if (scalarData->GetDataType() == DataType::Float)
            scalar = *(cpuData->DataBuffer<float>());
        else if (scalarData->GetDataType() == DataType::Double)
            scalar = *(cpuData->DataBuffer<double>());
        else
            LogicError("Unsupported DataType of training loss value");

        return scalar;
    }

    static void GetScalarValueAsyncBegin(const ValuePtr& value, double* pPinnedScalar)
    {
        if (value->Mask())
            LogicError("Scalar Value object cannot have an associated mask");

        auto scalarData = value->Data();
        if (scalarData->Shape().TotalSize() != 1)
            LogicError("Scalar Value object's has a size > 1");

        *pPinnedScalar = std::numeric_limits<double>::quiet_NaN();

        if (scalarData->Device() == DeviceDescriptor::CPUDevice())
        {
            // CPU device scalar, read directly
            if (scalarData->GetDataType() == DataType::Float)
                *pPinnedScalar = *(scalarData->DataBuffer<float>());
            else if (scalarData->GetDataType() == DataType::Double)
                *pPinnedScalar = *(scalarData->DataBuffer<double>());
            else
                LogicError("Unsupported DataType of training loss value");
        }
        else
        {
#ifdef CPUONLY
            LogicError("Unssported device");
#else
            // gpu device scalar, use cudaMemcpyAsync on pinned memory
            if (scalarData->GetDataType() == DataType::Float)
            {
                cudaMemcpyAsync(pPinnedScalar, scalarData->DataBuffer<float>(), sizeof(float), cudaMemcpyDeviceToHost);
            }
            else if (scalarData->GetDataType() == DataType::Double)
            {
                cudaMemcpyAsync(pPinnedScalar, scalarData->DataBuffer<double>(), sizeof(double), cudaMemcpyDeviceToHost);
            }
            else
                LogicError("Unsupported DataType of training loss value");
#endif
        }
    }

    static double GetScalarValueAsyncEnd(const ValuePtr& value, double* pPinnedScalar)
    {
        if (value->Mask())
            LogicError("Scalar Value object cannot have an associated mask");

        auto scalarData = value->Data();
        if (scalarData->Shape().TotalSize() != 1)
            LogicError("Scalar Value object's has a size > 1");

        if (scalarData->Device() == DeviceDescriptor::CPUDevice())
        {
            return *pPinnedScalar; // float to double conversion is done previously on CPU, no more works needed
        }
        else
        {
#ifdef CPUONLY
            LogicError("Unsupported device");
#else
            if (scalarData->GetDataType() == DataType::Float)
            {
                return (double)(*(float*)pPinnedScalar); // the memory for cudaMemcpyAsync is in float, cast to double
            }
            else if (scalarData->GetDataType() == DataType::Double)
            {
                return *pPinnedScalar;
            }
            else
                LogicError("Unsupported DataType of training loss value");
#endif
        }
    }

    static size_t GetSampleCount(const Variable& var, const ValuePtr& value)
    {
        auto valueDataShape = value->Shape();
        size_t numMaskedSamples = value->MaskedCount();
        size_t numSamplesInDataArrayView = valueDataShape.SubShape(var.Shape().Rank()).TotalSize();
        if (numMaskedSamples > numSamplesInDataArrayView)
            LogicError("Number of masked values cannot exceed the number of samples that the Value object's Data NDArrayView can hold");

        return (numSamplesInDataArrayView - numMaskedSamples);
    }

    static std::unordered_map<Variable, ValuePtr> GetInputs(const std::unordered_map<Variable, MinibatchData>& arguments)
    {
        std::unordered_map<Variable, ValuePtr> inputs(arguments.size());
        for (const auto& kv : arguments)
        {
            inputs[kv.first] = kv.second.data;
        }
        return inputs;
    }

    static bool IsAtSweepEnd(const std::unordered_map<Variable, MinibatchData>& arguments)
    {
        return std::any_of(arguments.begin(), arguments.end(), [](const std::pair<const Variable, MinibatchData>& kv)
        {
            return kv.second.sweepEnd;
        });
    }

    double Trainer::TestMinibatch(const std::unordered_map<Variable, MinibatchData>& arguments, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        return TestMinibatch(GetInputs(arguments), computeDevice);
    }

    double Trainer::TestMinibatch(const std::unordered_map<Variable, ValuePtr>& arguments, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        if (!m_aggregatedEvaluationFunction)
            InvalidArgument("Trainer::TestMinibatch: Cannot test when no evaluation function was specified during 'this' trainer's construction");

        // TODO: Should we refactor this code that is somewhat similar to the prologue of the TrainMinibatch function
        std::unordered_map<Variable, ValuePtr> outputs = { { m_aggregatedEvaluationFunction, nullptr }, { m_testSampleCountVar, nullptr } };

        m_combinedTrainingFunction->Forward(arguments, outputs, computeDevice);

        auto sampleCount = GetSampleCount(m_testSampleCountVar, outputs[m_testSampleCountVar]);
        return (GetScalarValue(outputs[m_aggregatedEvaluationFunction]) / sampleCount);
    }

    bool Trainer::TrainMinibatch(const std::unordered_map<Variable, MinibatchData>& arguments, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        std::unordered_map<Variable, ValuePtr> outputsToFetch = {};
        return TrainMinibatch(arguments, outputsToFetch, computeDevice);
    }

    bool Trainer::TrainMinibatch(const std::unordered_map<Variable, MinibatchData>& arguments, std::unordered_map<Variable, ValuePtr>& outputsToFetch, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        if (!m_distributed)
            return TrainLocalMinibatch(GetInputs(arguments), outputsToFetch, IsAtSweepEnd(arguments), computeDevice);
        return TrainDistributedMinibatch(GetInputs(arguments), outputsToFetch, IsAtSweepEnd(arguments), computeDevice);
    }

    bool Trainer::TrainMinibatch(const std::unordered_map<Variable, ValuePtr>& arguments, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        std::unordered_map<Variable, ValuePtr> outputsToFetch = {};
        return TrainMinibatch(arguments, outputsToFetch, computeDevice);
    }

    bool Trainer::TrainMinibatch(const std::unordered_map<Variable, ValuePtr>& arguments, std::unordered_map<Variable, ValuePtr>& outputsToFetch, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        if (!m_distributed)
            return TrainLocalMinibatch(arguments, outputsToFetch, false, computeDevice);
        return TrainDistributedMinibatch(arguments, outputsToFetch, false, computeDevice);
    }

    bool Trainer::TrainLocalMinibatch(const std::unordered_map<Variable, ValuePtr>& arguments, std::unordered_map<Variable, ValuePtr>& outputsToFetch, bool sweepEnd, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        bool emptyMinibatch = arguments.empty() || (arguments.begin()->second == nullptr);
        if (emptyMinibatch) // Nothing to train with.
            return false;

        std::unordered_map<Variable, ValuePtr> parameterGradients;
        ExecuteForwardBackward(arguments, outputsToFetch, computeDevice, parameterGradients);

        std::unordered_map<Parameter, NDArrayViewPtr> gradients;
        for (const auto& parameter : m_learnerParameters)
            gradients[parameter] = parameterGradients[parameter]->Data();
        return m_parameterLearners->Update(gradients, m_prevMinibatchNumSamples, sweepEnd);
    }

    bool Trainer::TrainDistributedMinibatch(const std::unordered_map<Variable, ValuePtr>& arguments, std::unordered_map<Variable, ValuePtr>& outputsToFetch, bool sweepEnd, const DeviceDescriptor& computeDevice /*= DeviceDescriptor::UseDefaultDevice()*/)
    {
        std::unordered_map<Parameter, NDArrayViewPtr> gradients;
        gradients.reserve(m_learnerParameters.size());

        bool emptyMinibatch = arguments.empty() || (arguments.begin()->second == nullptr);
        NDArrayViewPtr trainingLoss = nullptr;
        NDArrayViewPtr evalCriterion = nullptr;
        if (emptyMinibatch)
        {
            m_prevMinibatchNumSamples = 0;
            // Gradients are not existing.
            for (const auto& parameter : m_learnerParameters)
                gradients[parameter] = nullptr;
        }
        else
        {
            // Get gradients after forward/backward pass.
            std::unordered_map<Variable, ValuePtr> parameterGradients;
            ExecuteForwardBackward(arguments, outputsToFetch, computeDevice, parameterGradients);
            for (const auto& parameter : m_learnerParameters)
                gradients[parameter] = parameterGradients[parameter]->Data();
            trainingLoss = m_prevMinibatchAggregateTrainingLossValue->Data();
            evalCriterion = m_prevMinibatchAggregateEvalCriterionValue->Data();
        }

        MinibatchInfo info{ arguments.empty(), sweepEnd, m_prevMinibatchNumSamples, trainingLoss, evalCriterion };
        bool updated = m_parameterLearners->Update(gradients, info);
        m_prevMinibatchNumSamples = info.numberOfSamples;

        // Update internal state.
        if (emptyMinibatch)
        {
            // Have to reassign loss and criterion.
            m_prevMinibatchAggregateEvalCriterionValue = std::make_shared<Value>(info.evalCriterionValue);
            m_prevMinibatchAggregateTrainingLossValue = std::make_shared<Value>(info.trainingLossValue);
        }
        return updated;
    }

    void Trainer::ExecuteForwardBackward(const std::unordered_map<Variable, ValuePtr>& arguments, std::unordered_map<Variable, ValuePtr>& outputsToFetch, const DeviceDescriptor& computeDevice, std::unordered_map<Variable, ValuePtr>& parameterGradients)
    {
        std::unordered_map<Variable, ValuePtr> outputs = { { m_aggregatedLossFunction, nullptr }, { m_trainingSampleCountVar, nullptr } };
        if (m_aggregatedEvaluationFunction)
            outputs.insert({ m_aggregatedEvaluationFunction, nullptr });

        outputs.insert(outputsToFetch.begin(), outputsToFetch.end());

        auto backPropSate = m_combinedTrainingFunction->Forward(arguments, outputs, computeDevice, { m_aggregatedLossFunction }, m_modelParametersNotCoveredByLearners);
        m_prevMinibatchAggregateTrainingLossValue = outputs[m_aggregatedLossFunction];
        if (m_aggregatedEvaluationFunction)
            m_prevMinibatchAggregateEvalCriterionValue = outputs[m_aggregatedEvaluationFunction];

        // async copy of the scalar values if on GPU
        double* pTrainingLoss = nullptr;
        double* pEvalCriterion = nullptr;
        bool scalarOnGPU =
            (m_prevMinibatchAggregateTrainingLossValue->Device() != DeviceDescriptor::CPUDevice()) ||
            (m_aggregatedEvaluationFunction && m_prevMinibatchAggregateEvalCriterionValue->Device() != DeviceDescriptor::CPUDevice());

        if (scalarOnGPU)
        {
#ifdef CPUONLY
            LogicError("Unsupported device");
#else
            if (m_pAsyncScalarData == nullptr)
            {
                m_pAsyncScalarData = new TrainerAsyncScalarData;
            }
            auto pinnedScalars = TrainerAsyncScalarData::GetPinnedScalars(m_pAsyncScalarData);
            pTrainingLoss = &(pinnedScalars->m_trainingLoss);
            pEvalCriterion = &(pinnedScalars->m_evalCriterion);
#endif
        }
        else
        {
            pTrainingLoss = &m_prevMinibatchAverageTrainingLoss;
            pEvalCriterion = &m_prevMinibatchAverageEvalCriterion;
        }

        GetScalarValueAsyncBegin(m_prevMinibatchAggregateTrainingLossValue, pTrainingLoss);
        if (m_aggregatedEvaluationFunction)
        {
            GetScalarValueAsyncBegin(m_prevMinibatchAggregateEvalCriterionValue, pEvalCriterion);
        }

#ifndef CPUONLY
        if (scalarOnGPU) cudaEventRecord(TrainerAsyncScalarData::GetScalarDoneEvent(m_pAsyncScalarData));
#endif

        for (auto outputToFetch : outputsToFetch)
        {
            if (outputToFetch.second == nullptr)
                outputsToFetch[outputToFetch.first] = outputs[outputToFetch.first];
        }

        if(!m_rootGradientValue ||
            m_aggregatedLossFunction->Output().GetDataType() != m_rootGradientValue->GetDataType() ||
            m_prevMinibatchAggregateTrainingLossValue->Shape() != m_rootGradientValue->Shape() ||
            computeDevice != m_rootGradientValue->Device() ||
            outputs.at(m_aggregatedLossFunction)->Mask() != m_rootGradientValue->Mask())
        {
            m_rootGradientValue = MakeSharedObject<Value>(MakeSharedObject<NDArrayView>(m_aggregatedLossFunction->Output().GetDataType(), m_prevMinibatchAggregateTrainingLossValue->Shape(), computeDevice), outputs.at(m_aggregatedLossFunction)->Mask());
        }

        if (m_aggregatedLossFunction->Output().GetDataType() == DataType::Float)
            m_rootGradientValue->Data()->SetValue(1.0f);
        else
            m_rootGradientValue->Data()->SetValue(1.0);

        for (const auto& parameter : m_learnerParameters)
            parameterGradients[parameter] = nullptr;

        // TODO: Why Backward signature does not take Parameter instead of Variable for gradients?
        m_combinedTrainingFunction->Backward(backPropSate, { { m_aggregatedLossFunction, m_rootGradientValue } }, parameterGradients);
        m_prevMinibatchNumSamples = GetSampleCount(m_trainingSampleCountVar, outputs[m_trainingSampleCountVar]);

#ifndef CPUONLY
        if (scalarOnGPU) cudaEventSynchronize(TrainerAsyncScalarData::GetScalarDoneEvent(m_pAsyncScalarData));
#endif

        m_prevMinibatchAverageTrainingLoss = GetScalarValueAsyncEnd(m_prevMinibatchAggregateTrainingLossValue, pTrainingLoss) / m_prevMinibatchNumSamples;
        if (m_aggregatedEvaluationFunction)
        {
            m_prevMinibatchAverageEvalCriterion = GetScalarValueAsyncEnd(m_prevMinibatchAggregateEvalCriterionValue, pEvalCriterion) / m_prevMinibatchNumSamples;
        }
    }

    static std::wstring GetTrainerStateCheckpointFilePath(const std::wstring& modelFilePath)
    {
        const wchar_t* checkpointExt = L".ckp";
        return modelFilePath + checkpointExt;
    }

    void Trainer::SaveCheckpoint(const std::wstring& modelFilePath, Dictionary externalState)
    {
        auto learnersState = m_parameterLearners->CreateCheckpoint();
        if (!m_distributed)
            return Save(modelFilePath, learnersState, externalState);

        // Collect distrbuted external state.
        DistributedCommunicatorPtr communicator = MPICommunicator();
        communicator->Barrier();

        std::vector<DictionaryPtr> remoteState;
        communicator->Gather(externalState, remoteState, communicator->Workers());

        Dictionary aggregatedState;
        for (const auto& w : communicator->Workers())
        {
            aggregatedState[std::to_wstring(w.m_globalRank)] = *remoteState[w.m_globalRank];
        }

        if (communicator->CurrentWorker().IsMain())
            Save(modelFilePath, learnersState, aggregatedState);

        // all workers need to sync up after saving model to avoid read-after-write hazard
        // i.e. one worker is in the middle of write while another tries to read
        communicator->Barrier();
    }

    void Trainer::Save(const std::wstring& modelFilePath, const std::vector<DictionaryValue>& learnerState, const Dictionary& externalState)
    {
        std::wstring tempModelFile = modelFilePath + L".tmp";
        Dictionary state;
        state[learnersPropertyName] = learnerState;
        state[externalStatePropertyName] = externalState;

        m_combinedTrainingFunction->SaveModel(tempModelFile);
        std::wstring trainerStateCheckpointFilePath = GetTrainerStateCheckpointFilePath(modelFilePath);
        std::wstring tempCheckpointFile = trainerStateCheckpointFilePath + L".tmp";

        state.Save(tempCheckpointFile);

        // The return value is ignored here.
        _wunlink(modelFilePath.c_str());
        _wunlink(trainerStateCheckpointFilePath.c_str());

        renameOrDie(tempModelFile, modelFilePath);
        renameOrDie(tempCheckpointFile, trainerStateCheckpointFilePath);
    }

    Dictionary Trainer::RestoreFromCheckpoint(const std::wstring& modelFilePath)
    {
        // Restore the model's parameters
        m_combinedTrainingFunction->RestoreModel(modelFilePath);

        Dictionary checkpoint = Dictionary::Load(GetTrainerStateCheckpointFilePath(modelFilePath));

        auto learnerState = checkpoint[learnersPropertyName].Value<std::vector<DictionaryValue>>();
        auto externalState = checkpoint[externalStatePropertyName].Value<Dictionary>();

        if (!m_distributed)
        {
            m_parameterLearners->RestoreFromCheckpoint(learnerState);
            return externalState;
        }

        m_parameterLearners->RestoreFromCheckpoint(learnerState);
        DistributedCommunicatorPtr communicator = MPICommunicator();
        communicator->Barrier();

        auto key = std::to_wstring(communicator->CurrentWorker().m_globalRank);

        if (externalState.Contains(key))
            return externalState[key].Value<Dictionary>();
        else
            return externalState[std::to_wstring(0)].Value<Dictionary>();
    }

    const std::vector<LearnerPtr>& Trainer::ParameterLearners() const
    {
        return m_parameterLearners->ParameterLearners();
    }

    size_t Trainer::TotalNumberOfSamplesSeen() const
    {
        return m_parameterLearners->ParameterLearners().front()->TotalNumberOfSamplesSeen();
    }

    TrainerPtr CreateTrainer(const FunctionPtr& model, const FunctionPtr& lossFunction, const std::vector<LearnerPtr>& parameterLearners)
    {
        return MakeSharedObject<Trainer>(model, lossFunction, parameterLearners);
    }

    TrainerPtr CreateTrainer(const FunctionPtr& model, const FunctionPtr& lossFunction, const FunctionPtr& evaluationFunction, const std::vector<LearnerPtr>& parameterLearners)
    {
        return MakeSharedObject<Trainer>(model, lossFunction, evaluationFunction, parameterLearners);
    }
}
