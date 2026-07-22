#include "pch.h"
#include "GameCore.h"
#include "GraphicsCore.h"
#include "GraphicsCommon.h"
#include "SystemTime.h"
#include "TextRenderer.h"
#include "GameInput.h"
#include "CommandContext.h"
#include "PipelineState.h"
#include "BufferManager.h"
#include "ColorBuffer.h"
#include "GpuBuffer.h"
#include "ProgramDesc.h"
#include "ProgramBinder.h"
#include "ProgramManager.h"
#include "ReadbackBuffer.h"

using namespace GameCore;
using namespace Graphics;

namespace
{
    ComputePSO g_ProgramSmokeTestPSO(L"Program Smoke Test PSO");
    ComputePSO g_ProgramValuePackingTestPSO(L"Program Value Packing Test PSO");
    GraphicsPSO g_ProgramGraphicsBinderTestPSO(L"Program Graphics Binder Test PSO");
    ComputePSO g_ProgramDescriptorExecutionTestPSO(L"Program Descriptor Execution Test PSO");
    ComputePSO g_ProgramContextSwitchComputePSO(L"Program Context Switch Compute PSO");
    GraphicsPSO g_ProgramContextSwitchGraphicsPSO(L"Program Context Switch Graphics PSO");

    constexpr uint32_t kProgramValuePackingOutputCount = 128;

    std::string GetTestSourcePath(const char* relativePath)
    {
        return Utility::GetBasePath(std::string(__FILE__)) + relativePath;
    }

    std::shared_ptr<Program> BuildRequiredTestProgram(
        const ProgramDesc& desc,
        const char* testName)
    {
        std::string buildLog;
        std::shared_ptr<Program> program = ProgramManager::Get().GetProgram(desc, &buildLog);
        if (program != nullptr)
            return program;

        Utility::Printf("[%s] Program build failed:\n", testName);
        Utility::Print(buildLog.c_str());
        Utility::Print("\n");
        ASSERT(false);
        return nullptr;
    }

    const ProgramBinding* RequireTestBinding(
        const Program& program,
        const char* bindingName,
        const char* testName)
    {
        const ProgramBinding* binding = program.FindBinding(bindingName);
        if (binding != nullptr)
            return binding;

        Utility::Printf("[%s] Missing reflected binding '%s'.\n", testName, bindingName);
        ASSERT(false);
        return nullptr;
    }

    D3D12_SAMPLER_DESC MakeTestStaticSamplerDesc()
    {
        D3D12_SAMPLER_DESC desc = {};
        desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.MaxLOD = D3D12_FLOAT32_MAX;
        return desc;
    }

    void ConfigureGraphicsTestPSO(
        GraphicsPSO& pso,
        const Program& program,
        DXGI_FORMAT renderTargetFormat)
    {
        pso.SetRootSignature(program.GetRootSignature());
        pso.SetRasterizerState(RasterizerTwoSided);
        pso.SetBlendState(BlendDisable);
        pso.SetDepthStencilState(DepthStateDisabled);
        pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        pso.SetRenderTargetFormat(renderTargetFormat, DXGI_FORMAT_UNKNOWN);
        pso.SetVertexShader(program.GetD3D12Bytecode(ShaderStage::Vertex));
        pso.SetPixelShader(program.GetD3D12Bytecode(ShaderStage::Pixel));
        pso.Finalize();
    }

    uint32_t ReadbackSingleUint(ReadbackBuffer& readbackBuffer, const char* testName)
    {
        const uint32_t* values = static_cast<const uint32_t*>(readbackBuffer.Map());
        ASSERT(values != nullptr);
        if (values == nullptr)
            return 0;

        const uint32_t value = values[0];
        readbackBuffer.Unmap();
        Utility::Printf("[%s] Readback value %u.\n", testName, value);
        return value;
    }

    uint32_t FloatBits(float value)
    {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "Unexpected float size");
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    void AppendFloat(std::vector<uint32_t>& values, float value)
    {
        values.push_back(FloatBits(value));
    }

    void AppendMatrixRows(
        std::vector<uint32_t>& values,
        const float* columnMajorValues,
        uint32_t rowCount,
        uint32_t columnCount)
    {
        for (uint32_t row = 0; row < rowCount; ++row)
        {
            for (uint32_t column = 0; column < columnCount; ++column)
                AppendFloat(values, columnMajorValues[column * rowCount + row]);
        }
    }

    void RunProgramRootSignatureLayoutTest()
    {
        constexpr const char* kTestName = "ProgramRootSignatureLayoutTest";

        ProgramDesc desc;
        desc.SetSourceFile(GetTestSourcePath("Shaders\\ProgramRootSignatureLayoutTest.slang"))
            .AddEntryPoint(ShaderStage::Compute, "computeMain")
            .AddRootConstants("g_Root")
            .AddRootBufferSRV("g_RootSRV")
            .AddRootBufferUAV("g_RootUAV")
            .AddStaticSampler("g_StaticSampler", MakeTestStaticSamplerDesc());

        std::shared_ptr<Program> program = BuildRequiredTestProgram(desc, kTestName);
        if (program == nullptr)
            return;

        const ProgramBinding* cbv = RequireTestBinding(*program, "g_CBV", kTestName);
        const ProgramBinding* rootConstants = RequireTestBinding(*program, "g_Root", kTestName);
        const ProgramBinding* rootSRV = RequireTestBinding(*program, "g_RootSRV", kTestName);
        const ProgramBinding* rootUAV = RequireTestBinding(*program, "g_RootUAV", kTestName);
        const ProgramBinding* textureA = RequireTestBinding(*program, "g_TextureA", kTestName);
        const ProgramBinding* textureB = RequireTestBinding(*program, "g_TextureB", kTestName);
        const ProgramBinding* textureInAnotherSpace =
            RequireTestBinding(*program, "g_TextureInAnotherSpace", kTestName);
        const ProgramBinding* uavA = RequireTestBinding(*program, "g_UAVA", kTestName);
        const ProgramBinding* uavB = RequireTestBinding(*program, "g_UAVB", kTestName);
        const ProgramBinding* samplerA = RequireTestBinding(*program, "g_SamplerA", kTestName);
        const ProgramBinding* samplerB = RequireTestBinding(*program, "g_SamplerB", kTestName);
        const ProgramBinding* staticSampler =
            RequireTestBinding(*program, "g_StaticSampler", kTestName);
        if (cbv == nullptr || rootConstants == nullptr || rootSRV == nullptr || rootUAV == nullptr ||
            textureA == nullptr || textureB == nullptr || textureInAnotherSpace == nullptr ||
            uavA == nullptr || uavB == nullptr || samplerA == nullptr || samplerB == nullptr ||
            staticSampler == nullptr)
        {
            return;
        }

        ASSERT(cbv->Kind == ProgramBindingKind::ConstantBuffer);
        ASSERT(cbv->Register == 2 && cbv->Space == 1 && cbv->Count == 1);
        ASSERT(cbv->RootIndex == 0 && cbv->TableOffset == 0);

        ASSERT(rootConstants->Kind == ProgramBindingKind::RootConstants);
        // Slang places a ParameterBlock's ordinary data at b0 in the block's
        // SubElementRegisterSpace rather than at the container annotation.
        ASSERT(rootConstants->Register == 0 && rootConstants->Space == 4);
        ASSERT(rootConstants->RootIndex == 1 && rootConstants->Num32BitValues == 4);

        ASSERT(rootSRV->Kind == ProgramBindingKind::RootBufferSRV);
        ASSERT(rootSRV->Register == 3 && rootSRV->Space == 2);
        ASSERT(rootSRV->RootIndex == 2 && rootSRV->TableOffset == 0);

        ASSERT(rootUAV->Kind == ProgramBindingKind::RootBufferUAV);
        ASSERT(rootUAV->Register == 4 && rootUAV->Space == 2);
        ASSERT(rootUAV->RootIndex == 3 && rootUAV->TableOffset == 0);

        ASSERT(textureA->Kind == ProgramBindingKind::SRV);
        ASSERT(textureA->Register == 0 && textureA->Space == 0 && textureA->Count == 1);
        ASSERT(textureA->RootIndex == 4 && textureA->TableOffset == 0);
        ASSERT(textureB->Kind == ProgramBindingKind::SRV);
        ASSERT(textureB->Register == 1 && textureB->Space == 0 && textureB->Count == 2);
        ASSERT(textureB->RootIndex == 4 && textureB->TableOffset == 1);
        ASSERT(textureInAnotherSpace->Kind == ProgramBindingKind::SRV);
        ASSERT(textureInAnotherSpace->Register == 0 && textureInAnotherSpace->Space == 3);
        ASSERT(textureInAnotherSpace->RootIndex == 4 && textureInAnotherSpace->TableOffset == 3);

        ASSERT(uavA->Kind == ProgramBindingKind::UAV);
        ASSERT(uavA->Register == 0 && uavA->Space == 0 && uavA->Count == 1);
        ASSERT(uavA->RootIndex == 5 && uavA->TableOffset == 0);
        ASSERT(uavB->Kind == ProgramBindingKind::UAV);
        ASSERT(uavB->Register == 1 && uavB->Space == 0 && uavB->Count == 2);
        ASSERT(uavB->RootIndex == 5 && uavB->TableOffset == 1);

        ASSERT(samplerA->Kind == ProgramBindingKind::Sampler);
        ASSERT(samplerA->Register == 0 && samplerA->Space == 0 && samplerA->Count == 1);
        ASSERT(samplerA->RootIndex == 6 && samplerA->TableOffset == 0);
        ASSERT(samplerB->Kind == ProgramBindingKind::Sampler);
        ASSERT(samplerB->Register == 1 && samplerB->Space == 0 && samplerB->Count == 2);
        ASSERT(samplerB->RootIndex == 6 && samplerB->TableOffset == 1);
        ASSERT(staticSampler->Kind == ProgramBindingKind::StaticSampler);
        ASSERT(staticSampler->Register == 3 && staticSampler->Space == 0);
        ASSERT(staticSampler->RootIndex == UINT_MAX);

        const RootSignature& rootSignature = program->GetRootSignature();

        const D3D12_ROOT_PARAMETER1& cbvParameter = rootSignature[cbv->RootIndex]();
        ASSERT(cbvParameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV);
        ASSERT(cbvParameter.Descriptor.ShaderRegister == 2);
        ASSERT(cbvParameter.Descriptor.RegisterSpace == 1);

        const D3D12_ROOT_PARAMETER1& constantsParameter =
            rootSignature[rootConstants->RootIndex]();
        ASSERT(constantsParameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
        ASSERT(constantsParameter.Constants.ShaderRegister == 0);
        ASSERT(constantsParameter.Constants.RegisterSpace == 4);
        ASSERT(constantsParameter.Constants.Num32BitValues == 4);

        const D3D12_ROOT_PARAMETER1& srvParameter = rootSignature[rootSRV->RootIndex]();
        ASSERT(srvParameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV);
        ASSERT(srvParameter.Descriptor.ShaderRegister == 3);
        ASSERT(srvParameter.Descriptor.RegisterSpace == 2);

        const D3D12_ROOT_PARAMETER1& uavParameter = rootSignature[rootUAV->RootIndex]();
        ASSERT(uavParameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV);
        ASSERT(uavParameter.Descriptor.ShaderRegister == 4);
        ASSERT(uavParameter.Descriptor.RegisterSpace == 2);

        const D3D12_ROOT_PARAMETER1& srvTable = rootSignature[textureA->RootIndex]();
        ASSERT(srvTable.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
        ASSERT(srvTable.DescriptorTable.NumDescriptorRanges == 3);
        const D3D12_DESCRIPTOR_RANGE1* srvRanges = srvTable.DescriptorTable.pDescriptorRanges;
        ASSERT(srvRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
        ASSERT(srvRanges[0].BaseShaderRegister == 0 && srvRanges[0].RegisterSpace == 0);
        ASSERT(srvRanges[0].NumDescriptors == 1);
        ASSERT(srvRanges[1].BaseShaderRegister == 1 && srvRanges[1].RegisterSpace == 0);
        ASSERT(srvRanges[1].NumDescriptors == 2);
        ASSERT(srvRanges[2].BaseShaderRegister == 0 && srvRanges[2].RegisterSpace == 3);
        ASSERT(srvRanges[2].NumDescriptors == 1);

        const D3D12_ROOT_PARAMETER1& uavTable = rootSignature[uavA->RootIndex]();
        ASSERT(uavTable.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
        ASSERT(uavTable.DescriptorTable.NumDescriptorRanges == 2);
        const D3D12_DESCRIPTOR_RANGE1* uavRanges = uavTable.DescriptorTable.pDescriptorRanges;
        ASSERT(uavRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
        ASSERT(uavRanges[0].BaseShaderRegister == 0 && uavRanges[0].NumDescriptors == 1);
        ASSERT(uavRanges[1].BaseShaderRegister == 1 && uavRanges[1].NumDescriptors == 2);

        const D3D12_ROOT_PARAMETER1& samplerTable = rootSignature[samplerA->RootIndex]();
        ASSERT(samplerTable.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
        ASSERT(samplerTable.DescriptorTable.NumDescriptorRanges == 2);
        const D3D12_DESCRIPTOR_RANGE1* samplerRanges =
            samplerTable.DescriptorTable.pDescriptorRanges;
        ASSERT(samplerRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
        ASSERT(samplerRanges[0].BaseShaderRegister == 0 && samplerRanges[0].NumDescriptors == 1);
        ASSERT(samplerRanges[1].BaseShaderRegister == 1 && samplerRanges[1].NumDescriptors == 2);

        Utility::Printf(
            "[%s] Validated CBV/root constants/root descriptors and SRV/UAV/sampler tables.\n",
            kTestName);
    }

    void RunProgramRootSignatureLimitTests()
    {
        constexpr const char* kTestName = "ProgramRootSignatureLimitTest";
        const std::string source = GetTestSourcePath("Shaders\\ProgramRootSignatureLimitTest.slang");

        auto expectFailure = [&](uint32_t testCase, const char* expectedDiagnostic)
        {
            ProgramDesc desc;
            desc.SetSourceFile(source)
                .AddEntryPoint(ShaderStage::Compute, "computeMain")
                .AddDefine("ROOT_SIGNATURE_TEST_CASE", std::to_string(testCase))
                .AddRootBufferUAV("g_Output");
            if (testCase == 2)
                desc.AddRootConstants("g_Root");

            std::string buildLog;
            std::shared_ptr<Program> program = ProgramManager::Get().GetProgram(desc, &buildLog);
            ASSERT(program == nullptr);
            ASSERT(buildLog.find(expectedDiagnostic) != std::string::npos);
            if (program != nullptr || buildLog.find(expectedDiagnostic) == std::string::npos)
            {
                Utility::Printf(
                    "[%s] Case %u did not report expected diagnostic '%s':\n",
                    kTestName,
                    testCase,
                    expectedDiagnostic);
                Utility::Print(buildLog.c_str());
                Utility::Print("\n");
                return false;
            }
            return true;
        };

        const bool parameterLimitPassed = expectFailure(1, "exceeds 16 root parameters");
        const bool dwordLimitPassed = expectFailure(2, "D3D12 allows at most 64");
        const bool descriptorLimitPassed = expectFailure(3, "supports at most 32 per table");
        const bool overlapPassed = expectFailure(4, "Binding ranges overlap");
        ASSERT(parameterLimitPassed && dwordLimitPassed && descriptorLimitPassed && overlapPassed);

        Utility::Printf(
            "[%s] Validated root-parameter, DWORD, descriptor-table, and overlap failures.\n",
            kTestName);
    }

    void RunProgramGraphicsBinderTest()
    {
        constexpr const char* kTestName = "ProgramGraphicsBinderTest";
        constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        ProgramDesc desc;
        desc.SetSourceFile(GetTestSourcePath("Shaders\\ProgramGraphicsBinderTest.slang"))
            .AddEntryPoint(ShaderStage::Vertex, "vertexMain")
            .AddEntryPoint(ShaderStage::Pixel, "pixelMain")
            .AddRootBufferUAV("g_Output");

        std::shared_ptr<Program> program = BuildRequiredTestProgram(desc, kTestName);
        if (program == nullptr)
            return;
        ASSERT(program->HasBytecode(ShaderStage::Vertex));
        ASSERT(program->HasBytecode(ShaderStage::Pixel));

        ConfigureGraphicsTestPSO(g_ProgramGraphicsBinderTestPSO, *program, kRenderTargetFormat);

        ColorBuffer renderTarget;
        renderTarget.Create(L"Program Graphics Binder Test Target", 1, 1, 1, kRenderTargetFormat);
        StructuredBuffer outputBuffer;
        outputBuffer.Create(L"Program Graphics Binder Test Output", 1, sizeof(uint32_t));
        ReadbackBuffer readbackBuffer;
        readbackBuffer.Create(L"Program Graphics Binder Test Readback", 1, sizeof(uint32_t));

        GraphicsContext& context = GraphicsContext::Begin(L"Program Graphics Binder Test");
        context.TransitionResource(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
        context.TransitionResource(outputBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context.ClearColor(renderTarget);
        context.SetRenderTarget(renderTarget.GetRTV());
        context.SetViewportAndScissor(0, 0, 1, 1);
        context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ProgramBinder binder(*program, context);
        binder.SetRootSignature();
        context.SetPipelineState(g_ProgramGraphicsBinderTestPSO);
        binder["g_Constants"]["BaseValue"].Set(41u);
        binder["g_Texture"].SetSRV(GetDefaultTexture(kWhiteOpaque2D));
        binder["g_Sampler"].SetSampler(SamplerLinearClamp);
        binder["g_Output"].SetRootBufferUAV(outputBuffer);
        binder.Apply();
        context.Draw(3);
        context.CopyBuffer(readbackBuffer, outputBuffer);
        context.Finish(true);

        const uint32_t result = ReadbackSingleUint(readbackBuffer, kTestName);
        ASSERT(result == 42u);
        Utility::Printf("[%s] Validated VS/PS ProgramBinder output.\n", kTestName);
    }

    void RunProgramDescriptorExecutionTest()
    {
        constexpr const char* kTestName = "ProgramDescriptorExecutionTest";

        ProgramDesc desc;
        desc.SetSourceFile(GetTestSourcePath("Shaders\\ProgramDescriptorExecutionTest.slang"))
            .AddEntryPoint(ShaderStage::Compute, "computeMain")
            .AddRootBufferUAV("g_Output");

        std::shared_ptr<Program> program = BuildRequiredTestProgram(desc, kTestName);
        if (program == nullptr)
            return;

        const ProgramBinding* inputsBinding =
            RequireTestBinding(*program, "g_Inputs", kTestName);
        ASSERT(inputsBinding != nullptr);
        if (inputsBinding == nullptr)
            return;
        ASSERT(inputsBinding->Kind == ProgramBindingKind::SRV);
        ASSERT(inputsBinding->Count == 2);

        g_ProgramDescriptorExecutionTestPSO.SetRootSignature(program->GetRootSignature());
        g_ProgramDescriptorExecutionTestPSO.SetComputeShader(
            program->GetD3D12Bytecode(ShaderStage::Compute));
        g_ProgramDescriptorExecutionTestPSO.Finalize();

        alignas(16) const uint32_t firstInputData[4] = { 7u, 0u, 0u, 0u };
        alignas(16) const uint32_t secondInputData[4] = { 11u, 0u, 0u, 0u };
        StructuredBuffer firstInput;
        firstInput.Create(
            L"Program Descriptor Execution Test Input 0",
            1,
            sizeof(uint32_t),
            firstInputData);
        StructuredBuffer secondInput;
        secondInput.Create(
            L"Program Descriptor Execution Test Input 1",
            1,
            sizeof(uint32_t),
            secondInputData);
        StructuredBuffer outputBuffer;
        outputBuffer.Create(L"Program Descriptor Execution Test Output", 1, sizeof(uint32_t));
        ReadbackBuffer readbackBuffer;
        readbackBuffer.Create(L"Program Descriptor Execution Test Readback", 1, sizeof(uint32_t));

        ComputeContext& context = ComputeContext::Begin(L"Program Descriptor Execution Test");
        context.TransitionResource(firstInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(secondInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(outputBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ProgramBinder binder(*program, context);
        binder.SetRootSignature();
        context.SetPipelineState(g_ProgramDescriptorExecutionTestPSO);
        binder["g_Constants"]["Bias"].Set(3u);
        binder["g_Inputs"][0].SetSRV(firstInput.GetSRV());
        binder["g_Inputs"][1].SetSRV(secondInput.GetSRV());
        binder["g_Output"].SetRootBufferUAV(outputBuffer);
        binder.Apply();
        context.Dispatch(1, 1, 1);
        context.CopyBuffer(readbackBuffer, outputBuffer);
        context.Finish(true);

        const uint32_t result = ReadbackSingleUint(readbackBuffer, kTestName);
        ASSERT(result == 84u);
        Utility::Printf("[%s] Validated that both SRV array descriptors were consumed.\n", kTestName);
    }

    void RunProgramContextSwitchTest()
    {
        constexpr const char* kTestName = "ProgramContextSwitchTest";
        constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        const std::string source = GetTestSourcePath("Shaders\\ProgramContextSwitchTest.slang");

        ProgramDesc computeDesc;
        computeDesc.SetSourceFile(source)
            .AddEntryPoint(ShaderStage::Compute, "computeMain")
            .AddRootBufferUAV("g_Output");
        std::shared_ptr<Program> computeProgram =
            BuildRequiredTestProgram(computeDesc, kTestName);

        ProgramDesc graphicsDesc;
        graphicsDesc.SetSourceFile(source)
            .AddEntryPoint(ShaderStage::Vertex, "vertexMain")
            .AddEntryPoint(ShaderStage::Pixel, "pixelMain")
            .AddRootBufferUAV("g_Output");
        std::shared_ptr<Program> graphicsProgram =
            BuildRequiredTestProgram(graphicsDesc, kTestName);
        if (computeProgram == nullptr || graphicsProgram == nullptr)
            return;

        g_ProgramContextSwitchComputePSO.SetRootSignature(computeProgram->GetRootSignature());
        g_ProgramContextSwitchComputePSO.SetComputeShader(
            computeProgram->GetD3D12Bytecode(ShaderStage::Compute));
        g_ProgramContextSwitchComputePSO.Finalize();
        ConfigureGraphicsTestPSO(
            g_ProgramContextSwitchGraphicsPSO,
            *graphicsProgram,
            kRenderTargetFormat);

        StructuredBuffer computeOutput;
        computeOutput.Create(L"Program Context Switch Compute Output", 1, sizeof(uint32_t));
        StructuredBuffer graphicsOutput;
        graphicsOutput.Create(L"Program Context Switch Graphics Output", 1, sizeof(uint32_t));
        ReadbackBuffer computeReadback;
        computeReadback.Create(L"Program Context Switch Compute Readback", 1, sizeof(uint32_t));
        ReadbackBuffer graphicsReadback;
        graphicsReadback.Create(L"Program Context Switch Graphics Readback", 1, sizeof(uint32_t));
        ColorBuffer renderTarget;
        renderTarget.Create(L"Program Context Switch Target", 1, 1, 1, kRenderTargetFormat);

        GraphicsContext& graphicsContext = GraphicsContext::Begin(L"Program Context Switch Test");
        ComputeContext& computeContext = graphicsContext.GetComputeContext();

        computeContext.TransitionResource(computeOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ProgramBinder computeBinder(*computeProgram, computeContext);
        computeBinder.SetRootSignature();
        computeContext.SetPipelineState(g_ProgramContextSwitchComputePSO);
        computeBinder["g_Constants"]["Value"].Set(10u);
        computeBinder["g_Output"].SetRootBufferUAV(computeOutput);
        computeBinder.Apply();
        computeContext.Dispatch(1, 1, 1);

        graphicsContext.TransitionResource(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
        graphicsContext.TransitionResource(graphicsOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        graphicsContext.ClearColor(renderTarget);
        graphicsContext.SetRenderTarget(renderTarget.GetRTV());
        graphicsContext.SetViewportAndScissor(0, 0, 1, 1);
        graphicsContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ProgramBinder graphicsBinder(*graphicsProgram, graphicsContext);
        graphicsBinder.SetRootSignature();
        graphicsContext.SetPipelineState(g_ProgramContextSwitchGraphicsPSO);
        graphicsBinder["g_Constants"]["Value"].Set(20u);
        graphicsBinder["g_Output"].SetRootBufferUAV(graphicsOutput);
        graphicsBinder.Apply();
        graphicsContext.Draw(3);

        graphicsContext.CopyBuffer(computeReadback, computeOutput);
        graphicsContext.CopyBuffer(graphicsReadback, graphicsOutput);
        graphicsContext.Finish(true);

        const uint32_t computeResult = ReadbackSingleUint(computeReadback, kTestName);
        const uint32_t graphicsResult = ReadbackSingleUint(graphicsReadback, kTestName);
        ASSERT(computeResult == 11u);
        ASSERT(graphicsResult == 22u);
        Utility::Printf(
            "[%s] Validated compute (%u) to graphics (%u) root-state switching.\n",
            kTestName,
            computeResult,
            graphicsResult);
    }

    void RunProgramManagerCacheAndDiagnosticsTest()
    {
        constexpr const char* kTestName = "ProgramManagerCacheAndDiagnosticsTest";
        std::string source = GetTestSourcePath("Shaders\\ProgramDescriptorExecutionTest.slang");

        ProgramDesc desc;
        desc.SetSourceFile(source)
            .AddEntryPoint(ShaderStage::Compute, "computeMain")
            .AddRootBufferUAV("g_Output");
        std::shared_ptr<Program> first = BuildRequiredTestProgram(desc, kTestName);
        if (first == nullptr)
            return;

        std::string sourceWithForwardSlashes = source;
        for (char& ch : sourceWithForwardSlashes)
        {
            if (ch == '\\')
                ch = '/';
        }
        ProgramDesc equivalentDesc;
        equivalentDesc.SetSourceFile(sourceWithForwardSlashes)
            .AddRootBufferUAV("g_Output")
            .AddEntryPoint(ShaderStage::Compute, "computeMain");
        std::shared_ptr<Program> equivalent =
            BuildRequiredTestProgram(equivalentDesc, kTestName);
        ASSERT(equivalent == first);
        ASSERT(equivalent->GetVersionId() == first->GetVersionId());

        ProgramDesc variantDesc = desc;
        variantDesc.AddDefine("PROGRAM_CACHE_TEST_VARIANT", "1");
        std::shared_ptr<Program> variant = BuildRequiredTestProgram(variantDesc, kTestName);
        ASSERT(variant != nullptr && variant != first);
        ASSERT(variant->GetVersionId() > first->GetVersionId());

        ProgramDesc badEntryDesc;
        badEntryDesc.SetSourceFile(source)
            .AddEntryPoint(ShaderStage::Compute, "missingEntryPoint");
        std::string badEntryLog;
        std::shared_ptr<Program> badEntryProgram =
            ProgramManager::Get().GetProgram(badEntryDesc, &badEntryLog);
        ASSERT(badEntryProgram == nullptr);
        ASSERT(badEntryLog.find("Failed to find shader entry point") != std::string::npos);

        ProgramDesc missingSourceDesc;
        missingSourceDesc.SetSourceFile(GetTestSourcePath("Shaders\\MissingProgramTest.slang"))
            .AddEntryPoint(ShaderStage::Compute, "computeMain");
        std::string missingSourceLog;
        std::shared_ptr<Program> missingSourceProgram =
            ProgramManager::Get().GetProgram(missingSourceDesc, &missingSourceLog);
        ASSERT(missingSourceProgram == nullptr);
        ASSERT(missingSourceLog.find("Failed to load shader module") != std::string::npos);

        const uint64_t firstVersion = first->GetVersionId();
        ProgramManager::Get().ClearCache();
        std::shared_ptr<Program> afterClear = BuildRequiredTestProgram(desc, kTestName);
        ASSERT(afterClear != nullptr && afterClear != first);
        ASSERT(afterClear->GetVersionId() > firstVersion);

        Utility::Printf(
            "[%s] Validated cache reuse, variants, cache clear, and build diagnostics.\n",
            kTestName);
    }

    void RunProgramDescriptorHandleReflectionTest()
    {
        ProgramDesc desc;
        desc.SetSourceFile(GetTestSourcePath("Shaders\\ProgramDescriptorHandleTest.slang"))
            .AddEntryPoint(ShaderStage::Compute, "computeMain");

        std::string buildLog;
        std::shared_ptr<Program> program = ProgramManager::Get().GetProgram(desc, &buildLog);
        if (!buildLog.empty())
        {
            Utility::Print("[ProgramDescriptorHandleTest] Build log:\n");
            Utility::Print(buildLog.c_str());
            Utility::Print("\n");
        }

        if (program == nullptr || !program->HasBytecode(ShaderStage::Compute))
        {
            ERROR("Program descriptor handle test failed to build compute bytecode.");
            ASSERT(false);
            return;
        }

        const ProgramBinding* constantsBinding = program->FindBinding("g_Constants");
        const ProgramBinding* outputBinding = program->FindBinding("g_Output");
        ASSERT(constantsBinding != nullptr);
        ASSERT(outputBinding != nullptr);
        if (constantsBinding == nullptr || outputBinding == nullptr)
            return;

        ASSERT(constantsBinding->Kind == ProgramBindingKind::ConstantBuffer);
        ASSERT(outputBinding->Kind == ProgramBindingKind::UAV);

        const ProgramParameter* handle =
            program->FindParameter("g_Constants.TextureHandle");
        ASSERT(handle != nullptr);
        if (handle == nullptr)
            return;

        ASSERT(handle->Kind == ProgramParameterKind::Uniform);
        ASSERT(handle->UniformSize == sizeof(uint32_t) * 2);
        ASSERT(handle->RowCount == 1);
        ASSERT(handle->ColumnCount == 2);

        Utility::Printf(
            "[ProgramDescriptorHandleTest] Validated DescriptorHandle reflection.\n");
    }

    void RunProgramValuePackingTest()
    {
        ProgramDesc desc;
        desc.SetSourceFile(GetTestSourcePath("Shaders\\ProgramValuePackingTest.slang"))
            .AddEntryPoint(ShaderStage::Compute, "computeMain")
            .AddRootConstants("g_Root");

        std::string buildLog;
        std::shared_ptr<Program> program = ProgramManager::Get().GetProgram(desc, &buildLog);
        if (!buildLog.empty())
        {
            Utility::Print("[ProgramValuePackingTest] Build log:\n");
            Utility::Print(buildLog.c_str());
            Utility::Print("\n");
        }

        if (program == nullptr || !program->HasBytecode(ShaderStage::Compute))
        {
            ERROR("Program value packing test failed to build compute bytecode.");
            ASSERT(false);
            return;
        }

        const ProgramBinding* constantsBinding = program->FindBinding("g_Constants");
        const ProgramBinding* blockBinding = program->FindBinding("g_Block");
        const ProgramBinding* rootBinding = program->FindBinding("g_Root");
        const ProgramBinding* outputBinding = program->FindBinding("g_Output");
        ASSERT(constantsBinding != nullptr);
        ASSERT(blockBinding != nullptr);
        ASSERT(rootBinding != nullptr);
        ASSERT(outputBinding != nullptr);
        if (constantsBinding == nullptr || blockBinding == nullptr ||
            rootBinding == nullptr || outputBinding == nullptr)
        {
            return;
        }
        ASSERT(constantsBinding->Kind == ProgramBindingKind::ConstantBuffer);
        ASSERT(blockBinding->Kind == ProgramBindingKind::ConstantBuffer);
        ASSERT(rootBinding->Kind == ProgramBindingKind::RootConstants);
        ASSERT(outputBinding->Kind == ProgramBindingKind::UAV);

        const ProgramParameter* xmf3 = program->FindParameter("g_Constants.XMFLOAT3Value");
        const ProgramParameter* xmf3Guard = program->FindParameter("g_Constants.XMFLOAT3Guard");
        const ProgramParameter* xmi2 = program->FindParameter("g_Constants.XMINT2Value");
        const ProgramParameter* xmi3 = program->FindParameter("g_Constants.XMINT3Value");
        const ProgramParameter* xmi4 = program->FindParameter("g_Constants.XMINT4Value");
        const ProgramParameter* xmui2 = program->FindParameter("g_Constants.XMUINT2Value");
        const ProgramParameter* xmui3 = program->FindParameter("g_Constants.XMUINT3Value");
        const ProgramParameter* xmui4 = program->FindParameter("g_Constants.XMUINT4Value");
        const ProgramParameter* vector3 = program->FindParameter("g_Constants.Vector3Value");
        const ProgramParameter* vector3Guard = program->FindParameter("g_Constants.Vector3Guard");
        const ProgramParameter* floatArray = program->FindParameter("g_Constants.FloatArray");
        const ProgramParameter* float2Array = program->FindParameter("g_Constants.Float2Array");
        const ProgramParameter* vector3Array = program->FindParameter("g_Constants.Vector3Array");
        const ProgramParameter* columnMatrix = program->FindParameter("g_Constants.ColumnMatrix3");
        const ProgramParameter* columnMatrixGuard =
            program->FindParameter("g_Constants.ColumnMatrix3Guard");
        const ProgramParameter* rowMatrix = program->FindParameter("g_Constants.RowMatrix3");
        const ProgramParameter* rowMatrixGuard =
            program->FindParameter("g_Constants.RowMatrix3Guard");
        const ProgramParameter* matrix4 = program->FindParameter("g_Constants.Matrix4Value");
        const ProgramParameter* matrix3Array = program->FindParameter("g_Constants.Matrix3Array");
        const ProgramParameter* handle = program->FindParameter("g_Constants.Handle");
        const ProgramParameter* nestedArray = program->FindParameter("g_Block.NestedArray");

        ASSERT(xmf3 != nullptr && xmf3Guard != nullptr);
        ASSERT(xmi2 != nullptr && xmi3 != nullptr && xmi4 != nullptr);
        ASSERT(xmui2 != nullptr && xmui3 != nullptr && xmui4 != nullptr);
        ASSERT(vector3 != nullptr && vector3Guard != nullptr);
        ASSERT(floatArray != nullptr && float2Array != nullptr && vector3Array != nullptr);
        ASSERT(columnMatrix != nullptr && columnMatrixGuard != nullptr);
        ASSERT(rowMatrix != nullptr && rowMatrixGuard != nullptr);
        ASSERT(matrix4 != nullptr && matrix3Array != nullptr);
        ASSERT(handle != nullptr && nestedArray != nullptr);
        if (xmf3 == nullptr || xmf3Guard == nullptr || xmi2 == nullptr ||
            xmi3 == nullptr || xmi4 == nullptr || xmui2 == nullptr || xmui3 == nullptr ||
            xmui4 == nullptr || vector3 == nullptr || vector3Guard == nullptr ||
            floatArray == nullptr || float2Array == nullptr || vector3Array == nullptr ||
            columnMatrix == nullptr || columnMatrixGuard == nullptr || rowMatrix == nullptr ||
            rowMatrixGuard == nullptr || matrix4 == nullptr || matrix3Array == nullptr ||
            handle == nullptr || nestedArray == nullptr)
        {
            return;
        }

        ASSERT(xmi2->UniformSize == sizeof(int32_t) * 2);
        ASSERT(xmi2->RowCount == 1 && xmi2->ColumnCount == 2);
        ASSERT(xmi3->UniformSize == sizeof(int32_t) * 3);
        ASSERT(xmi3->RowCount == 1 && xmi3->ColumnCount == 3);
        ASSERT(xmi4->UniformSize == sizeof(int32_t) * 4);
        ASSERT(xmi4->RowCount == 1 && xmi4->ColumnCount == 4);
        ASSERT(xmui2->UniformSize == sizeof(uint32_t) * 2);
        ASSERT(xmui2->RowCount == 1 && xmui2->ColumnCount == 2);
        ASSERT(xmui3->UniformSize == sizeof(uint32_t) * 3);
        ASSERT(xmui3->RowCount == 1 && xmui3->ColumnCount == 3);
        ASSERT(xmui4->UniformSize == sizeof(uint32_t) * 4);
        ASSERT(xmui4->RowCount == 1 && xmui4->ColumnCount == 4);
        ASSERT(xmf3->UniformSize == sizeof(float) * 3);
        ASSERT(xmf3Guard->RelativeUniformOffset ==
            xmf3->RelativeUniformOffset + xmf3->UniformSize);
        ASSERT(vector3->UniformSize == sizeof(float) * 3);
        ASSERT(vector3Guard->RelativeUniformOffset ==
            vector3->RelativeUniformOffset + vector3->UniformSize);
        ASSERT(floatArray->UniformStride == sizeof(float) * 4);
        ASSERT(float2Array->UniformStride == sizeof(float) * 4);
        ASSERT(vector3Array->UniformStride == sizeof(float) * 4);
        ASSERT(matrix3Array->UniformStride == sizeof(float) * 12);
        ASSERT(nestedArray->UniformStride == sizeof(float) * 4);

        ASSERT(columnMatrix->UniformSize == 44);
        ASSERT(columnMatrix->UniformTypeStride == sizeof(float) * 12);
        ASSERT(columnMatrix->MatrixLayout == ProgramMatrixLayout::ColumnMajor);
        ASSERT(columnMatrix->MatrixVectorStride == sizeof(float) * 4);
        ASSERT(columnMatrixGuard->RelativeUniformOffset ==
            columnMatrix->RelativeUniformOffset + columnMatrix->UniformSize);

        ASSERT(rowMatrix->UniformSize == 44);
        ASSERT(rowMatrix->UniformTypeStride == sizeof(float) * 12);
        ASSERT(rowMatrix->MatrixLayout == ProgramMatrixLayout::RowMajor);
        ASSERT(rowMatrix->MatrixVectorStride == sizeof(float) * 4);
        ASSERT(rowMatrixGuard->RelativeUniformOffset ==
            rowMatrix->RelativeUniformOffset + rowMatrix->UniformSize);

        ASSERT(matrix4->UniformSize == sizeof(float) * 16);
        ASSERT(matrix4->UniformTypeStride == sizeof(float) * 16);
        ASSERT(handle->UniformSize == sizeof(uint32_t) * 2);

        g_ProgramValuePackingTestPSO.SetRootSignature(program->GetRootSignature());
        g_ProgramValuePackingTestPSO.SetComputeShader(
            program->GetD3D12Bytecode(ShaderStage::Compute));
        g_ProgramValuePackingTestPSO.Finalize();

        const float columnMatrixValues[9] = {
            101.0f, 102.0f, 103.0f,
            104.0f, 105.0f, 106.0f,
            107.0f, 108.0f, 109.0f
        };
        const float rowMatrixValues[9] = {
            111.0f, 112.0f, 113.0f,
            114.0f, 115.0f, 116.0f,
            117.0f, 118.0f, 119.0f
        };
        const float matrix4Values[16] = {
            121.0f, 122.0f, 123.0f, 124.0f,
            125.0f, 126.0f, 127.0f, 128.0f,
            129.0f, 130.0f, 131.0f, 132.0f,
            133.0f, 134.0f, 135.0f, 136.0f
        };
        const float matrix3Array0Values[9] = {
            141.0f, 142.0f, 143.0f,
            144.0f, 145.0f, 146.0f,
            147.0f, 148.0f, 149.0f
        };
        const float matrix3Array1Values[9] = {
            151.0f, 152.0f, 153.0f,
            154.0f, 155.0f, 156.0f,
            157.0f, 158.0f, 159.0f
        };

        const Math::Matrix3 columnMatrixValue(
            Math::Vector3(columnMatrixValues[0], columnMatrixValues[1], columnMatrixValues[2]),
            Math::Vector3(columnMatrixValues[3], columnMatrixValues[4], columnMatrixValues[5]),
            Math::Vector3(columnMatrixValues[6], columnMatrixValues[7], columnMatrixValues[8]));
        const Math::Matrix3 rowMatrixValue(
            Math::Vector3(rowMatrixValues[0], rowMatrixValues[1], rowMatrixValues[2]),
            Math::Vector3(rowMatrixValues[3], rowMatrixValues[4], rowMatrixValues[5]),
            Math::Vector3(rowMatrixValues[6], rowMatrixValues[7], rowMatrixValues[8]));
        const Math::Matrix4 matrix4Value(
            Math::Vector4(matrix4Values[0], matrix4Values[1], matrix4Values[2], matrix4Values[3]),
            Math::Vector4(matrix4Values[4], matrix4Values[5], matrix4Values[6], matrix4Values[7]),
            Math::Vector4(matrix4Values[8], matrix4Values[9], matrix4Values[10], matrix4Values[11]),
            Math::Vector4(matrix4Values[12], matrix4Values[13], matrix4Values[14], matrix4Values[15]));
        const Math::Matrix3 matrix3Array0(
            Math::Vector3(matrix3Array0Values[0], matrix3Array0Values[1], matrix3Array0Values[2]),
            Math::Vector3(matrix3Array0Values[3], matrix3Array0Values[4], matrix3Array0Values[5]),
            Math::Vector3(matrix3Array0Values[6], matrix3Array0Values[7], matrix3Array0Values[8]));
        const Math::Matrix3 matrix3Array1(
            Math::Vector3(matrix3Array1Values[0], matrix3Array1Values[1], matrix3Array1Values[2]),
            Math::Vector3(matrix3Array1Values[3], matrix3Array1Values[4], matrix3Array1Values[5]),
            Math::Vector3(matrix3Array1Values[6], matrix3Array1Values[7], matrix3Array1Values[8]));

        std::vector<uint32_t> expectedValues;
        expectedValues.reserve(kProgramValuePackingOutputCount);
        AppendFloat(expectedValues, 1.25f);
        expectedValues.push_back(static_cast<uint32_t>(static_cast<int32_t>(-17)));
        expectedValues.push_back(0x89abcdefu);
        expectedValues.push_back(1u);
        for (int32_t value : { -21, 22 })
            expectedValues.push_back(static_cast<uint32_t>(value));
        for (uint32_t value : { 0x10203040u, 0x50607080u })
            expectedValues.push_back(value);
        for (int32_t value : { -31, 32, -33 })
            expectedValues.push_back(static_cast<uint32_t>(value));
        for (uint32_t value : { 41u, 42u, 43u })
            expectedValues.push_back(value);
        for (int32_t value : { -51, 52, -53, 54 })
            expectedValues.push_back(static_cast<uint32_t>(value));
        for (uint32_t value : { 61u, 62u, 63u, 64u })
            expectedValues.push_back(value);
        AppendFloat(expectedValues, -2.5f);
        AppendFloat(expectedValues, 3.25f);
        AppendFloat(expectedValues, -4.5f);
        AppendFloat(expectedValues, 700.0f);
        AppendFloat(expectedValues, 5.0f);
        AppendFloat(expectedValues, 6.0f);
        AppendFloat(expectedValues, 7.0f);
        AppendFloat(expectedValues, 701.0f);
        AppendFloat(expectedValues, 8.0f);
        AppendFloat(expectedValues, 9.0f);
        AppendFloat(expectedValues, 10.0f);
        AppendFloat(expectedValues, 702.0f);
        for (float value : { 11.0f, 12.0f, 13.0f, 14.0f }) AppendFloat(expectedValues, value);
        for (float value : { 15.0f, 16.0f, 17.0f, 18.0f }) AppendFloat(expectedValues, value);
        for (float value : { 19.0f, 20.0f }) AppendFloat(expectedValues, value);
        for (float value : { 21.0f, 22.0f, 23.0f, 24.0f }) AppendFloat(expectedValues, value);
        for (float value : { 25.0f, 26.0f, 27.0f, 28.0f, 29.0f, 30.0f }) AppendFloat(expectedValues, value);
        AppendMatrixRows(expectedValues, columnMatrixValues, 3, 3);
        AppendFloat(expectedValues, 703.0f);
        AppendMatrixRows(expectedValues, rowMatrixValues, 3, 3);
        AppendFloat(expectedValues, 704.0f);
        AppendMatrixRows(expectedValues, matrix4Values, 4, 4);
        AppendMatrixRows(expectedValues, matrix3Array0Values, 3, 3);
        AppendMatrixRows(expectedValues, matrix3Array1Values, 3, 3);
        expectedValues.push_back(0x12345678u);
        expectedValues.push_back(0x9abcdef0u);
        for (float value : { 161.0f, 162.0f, 163.0f, 705.0f }) AppendFloat(expectedValues, value);
        for (float value : { 164.0f, 165.0f, 166.0f, 706.0f }) AppendFloat(expectedValues, value);
        for (float value : { 167.0f, 168.0f, 169.0f, 170.0f }) AppendFloat(expectedValues, value);
        for (float value : { 171.0f, 172.0f, 173.0f, 707.0f }) AppendFloat(expectedValues, value);
        expectedValues.push_back(0x2468ace0u);
        expectedValues.push_back(1u);
        ASSERT(expectedValues.size() == kProgramValuePackingOutputCount);
        if (expectedValues.size() != kProgramValuePackingOutputCount)
        {
            ERROR("Program value packing test has an invalid CPU expectation count.");
            return;
        }

        StructuredBuffer outputBuffer;
        outputBuffer.Create(
            L"Program Value Packing Test Output",
            kProgramValuePackingOutputCount,
            sizeof(uint32_t));
        ReadbackBuffer readbackBuffer;
        readbackBuffer.Create(
            L"Program Value Packing Test Readback",
            kProgramValuePackingOutputCount,
            sizeof(uint32_t));

        ComputeContext& context = ComputeContext::Begin(L"Program Value Packing Test");
        ProgramBinder binder(*program, context);
        binder.SetRootSignature();
        context.SetPipelineState(g_ProgramValuePackingTestPSO);
        context.TransitionResource(outputBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ProgramVar constants = binder["g_Constants"];
        constants["FloatValue"].Set(1.25f);
        constants["IntValue"].Set(static_cast<int32_t>(-17));
        constants["UIntValue"].Set(0x89abcdefu);
        constants["BoolValue"].Set(true);
        constants["XMINT2Value"].Set(DirectX::XMINT2(-21, 22));
        constants["XMUINT2Value"].Set(DirectX::XMUINT2(0x10203040u, 0x50607080u));
        constants["XMINT3Value"].Set(DirectX::XMINT3(-31, 32, -33));
        constants["XMUINT3Value"].Set(DirectX::XMUINT3(41u, 42u, 43u));
        constants["XMINT4Value"].Set(DirectX::XMINT4(-51, 52, -53, 54));
        constants["XMUINT4Value"].Set(DirectX::XMUINT4(61u, 62u, 63u, 64u));
        constants["ScalarValue"].Set(Math::Scalar(-2.5f));
        constants["XMFLOAT2Padding"].Set(700.0f);
        constants["XMFLOAT2Value"].Set(DirectX::XMFLOAT2(3.25f, -4.5f));
        constants["XMFLOAT3Guard"].Set(701.0f);
        constants["XMFLOAT3Value"].Set(DirectX::XMFLOAT3(5.0f, 6.0f, 7.0f));
        constants["Vector3Value"].Set(Math::Vector3(8.0f, 9.0f, 10.0f));
        constants["Vector3Guard"].Set(702.0f);
        constants["XMFLOAT4Value"].Set(DirectX::XMFLOAT4(11.0f, 12.0f, 13.0f, 14.0f));
        constants["Vector4Value"].Set(Math::Vector4(15.0f, 16.0f, 17.0f, 18.0f));
        constants["FloatArray"][0].Set(19.0f);
        constants["FloatArray"][1].Set(20.0f);
        constants["Float2Array"][0].Set(DirectX::XMFLOAT2(21.0f, 22.0f));
        constants["Float2Array"][1].Set(DirectX::XMFLOAT2(23.0f, 24.0f));
        constants["Vector3Array"][0].Set(Math::Vector3(25.0f, 26.0f, 27.0f));
        constants["Vector3Array"][1].Set(Math::Vector3(28.0f, 29.0f, 30.0f));
        constants["ColumnMatrix3Guard"].Set(703.0f);
        constants["ColumnMatrix3"].Set(columnMatrixValue);
        constants["RowMatrix3"].Set(rowMatrixValue);
        constants["RowMatrix3Guard"].Set(704.0f);
        constants["Matrix4Value"].Set(matrix4Value);
        constants["Matrix3Array"][0].Set(matrix3Array0);
        constants["Matrix3Array"][1].Set(matrix3Array1);
        constants["Handle"].Set(SlangDescriptorHandle{ 0x12345678u, 0x9abcdef0u });

        ProgramVar block = binder["g_Block"];
        block["NestedArray"][0]["Guard"].Set(705.0f);
        block["NestedArray"][0]["Value"].Set(Math::Vector3(161.0f, 162.0f, 163.0f));
        block["NestedArray"][1]["Value"].Set(Math::Vector3(164.0f, 165.0f, 166.0f));
        block["NestedArray"][1]["Guard"].Set(706.0f);
        block["Float2Array"][0].Set(DirectX::XMFLOAT2(167.0f, 168.0f));
        block["Float2Array"][1].Set(DirectX::XMFLOAT2(169.0f, 170.0f));

        ProgramVar root = binder["g_Root"];
        root["Guard"].Set(707.0f);
        root["Value"].Set(Math::Vector3(171.0f, 172.0f, 173.0f));
        root["UIntValue"].Set(0x2468ace0u);
        root["BoolValue"].Set(true);

        binder.SetUAV("g_Output", outputBuffer.GetUAV());
        binder.Apply();
        context.Dispatch(1, 1, 1);
        context.CopyBuffer(readbackBuffer, outputBuffer);
        context.Finish(true);

        const uint32_t* outputValues = static_cast<const uint32_t*>(readbackBuffer.Map());
        ASSERT(outputValues != nullptr);
        bool succeeded = outputValues != nullptr;
        if (outputValues != nullptr)
        {
            for (uint32_t index = 0; index < kProgramValuePackingOutputCount; ++index)
            {
                if (outputValues[index] == expectedValues[index])
                    continue;

                Utility::Printf(
                    "[ProgramValuePackingTest] Mismatch at output[%u]: expected=0x%08x actual=0x%08x\n",
                    index,
                    expectedValues[index],
                    outputValues[index]);
                succeeded = false;
                break;
            }
        }
        if (outputValues != nullptr)
            readbackBuffer.Unmap();
        if (!succeeded)
        {
            ERROR("Program value packing test failed GPU readback validation.");
            ASSERT(false);
            return;
        }

        Utility::Printf(
            "[ProgramValuePackingTest] Validated %u packed values across CBV, ParameterBlock, and root constants.\n",
            kProgramValuePackingOutputCount);
    }

    void RunProgramSmokeTest()
    {
        ProgramDesc defineOrderA;
        defineOrderA.AddDefine("PROGRAM_SMOKE_TEST_BONUS", "5")
            .AddDefine("PROGRAM_SMOKE_TEST_MODE", "1");
        ProgramDesc defineOrderB;
        defineOrderB.AddDefine("PROGRAM_SMOKE_TEST_MODE", "1")
            .AddDefine("PROGRAM_SMOKE_TEST_BONUS", "5");
        ASSERT(defineOrderA.GetCacheKey() == defineOrderB.GetCacheKey());

        D3D12_SAMPLER_DESC staticSamplerDesc = {};
        staticSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSamplerDesc.MaxAnisotropy = 1;
        staticSamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        staticSamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

        std::string smokeTestSource = GetTestSourcePath("Shaders\\ProgramSmokeTest.slang");
        std::string smokeTestSourceWithForwardSlashes = smokeTestSource;
        for (char& ch : smokeTestSourceWithForwardSlashes)
        {
            if (ch == '\\')
                ch = '/';
        }

        ProgramDesc pathKeyA;
        pathKeyA.SetSourceFile(smokeTestSource)
            .AddEntryPoint(ShaderStage::Compute, "computeMain");
        ProgramDesc pathKeyB;
        pathKeyB.SetSourceFile(smokeTestSourceWithForwardSlashes)
            .AddEntryPoint(ShaderStage::Compute, "computeMain");
        ASSERT(pathKeyA.GetCacheKey() == pathKeyB.GetCacheKey());

        ProgramDesc unorderedKeyA;
        unorderedKeyA.SetSourceFile(smokeTestSource)
            .AddEntryPoint(ShaderStage::Vertex, "vertexMain")
            .AddEntryPoint(ShaderStage::Pixel, "pixelMain")
            .AddRootConstants("g_Material")
            .AddRootConstants("g_Frame")
            .AddStaticSampler("g_Material.TextureParams.StaticSampler", staticSamplerDesc)
            .AddStaticSampler("g_Frame.StaticSampler", staticSamplerDesc);
        ProgramDesc unorderedKeyB;
        unorderedKeyB.SetSourceFile(smokeTestSourceWithForwardSlashes)
            .AddStaticSampler("g_Frame.StaticSampler", staticSamplerDesc)
            .AddStaticSampler("g_Material.TextureParams.StaticSampler", staticSamplerDesc)
            .AddRootConstants("g_Frame")
            .AddRootConstants("g_Material")
            .AddEntryPoint(ShaderStage::Pixel, "pixelMain")
            .AddEntryPoint(ShaderStage::Vertex, "vertexMain");
        ASSERT(unorderedKeyA.GetCacheKey() == unorderedKeyB.GetCacheKey());

        ProgramDesc includeOrderA;
        includeOrderA.AddIncludeDirectory("Shaders\\A")
            .AddIncludeDirectory("Shaders\\B");
        ProgramDesc includeOrderB;
        includeOrderB.AddIncludeDirectory("Shaders\\B")
            .AddIncludeDirectory("Shaders\\A");
        ASSERT(includeOrderA.GetCacheKey() != includeOrderB.GetCacheKey());

        ProgramDesc entryOverride;
        entryOverride.AddEntryPoint(ShaderStage::Compute, "oldMain")
            .AddEntryPoint(ShaderStage::Compute, "computeMain");
        ASSERT(entryOverride.GetEntryPoints().size() == 1);
        ASSERT(entryOverride.GetEntryPoints()[0].Name == "computeMain");

        ProgramDesc delimiterKeyA;
        delimiterKeyA.AddDefine("FOO", "1")
            .AddRootConstants("g_Root");
        ProgramDesc delimiterKeyB;
        delimiterKeyB.AddDefine("FOO", "1\nrootConstants:6:g_Root");
        ASSERT(delimiterKeyA.GetCacheKey() != delimiterKeyB.GetCacheKey());

        ProgramDesc rootBufferOrderA;
        rootBufferOrderA.AddRootBufferSRV("g_Input")
            .AddRootBufferUAV("g_Output");
        ProgramDesc rootBufferOrderB;
        rootBufferOrderB.AddRootBufferUAV("g_Output")
            .AddRootBufferSRV("g_Input");
        ASSERT(rootBufferOrderA.GetCacheKey() == rootBufferOrderB.GetCacheKey());

        ProgramDesc desc;
        desc.SetSourceFile(smokeTestSource)
            .AddEntryPoint(ShaderStage::Compute, "computeMain")
            .AddDefine("PROGRAM_SMOKE_TEST_BONUS", "5")
            .AddRootConstants("g_Material")
            .AddRootBufferSRV("g_Input")
            .AddRootBufferUAV("g_Output")
            .AddStaticSampler("g_Material.TextureParams.StaticSampler", staticSamplerDesc);

        ProgramDesc rootTextureDesc = desc;
        rootTextureDesc.AddRootBufferSRV("g_Nested.OuterTexture");
        std::string rootTextureBuildLog;
        std::shared_ptr<Program> rootTextureProgram =
            ProgramManager::Get().GetProgram(rootTextureDesc, &rootTextureBuildLog);
        ASSERT(rootTextureProgram == nullptr);
        ASSERT(rootTextureBuildLog.find("RootBufferSRV") != std::string::npos);
        ASSERT(rootTextureBuildLog.find("not a buffer resource") != std::string::npos);

        ProgramDesc rootArrayDesc = desc;
        rootArrayDesc.AddRootBufferSRV("g_InputArray");
        std::string rootArrayBuildLog;
        std::shared_ptr<Program> rootArrayProgram =
            ProgramManager::Get().GetProgram(rootArrayDesc, &rootArrayBuildLog);
        ASSERT(rootArrayProgram == nullptr);
        ASSERT(rootArrayBuildLog.find("RootBufferSRV") != std::string::npos);
        ASSERT(rootArrayBuildLog.find("must not be an array") != std::string::npos);

        std::string buildLog;
        std::shared_ptr<Program> program = ProgramManager::Get().GetProgram(desc, &buildLog);
        if (!buildLog.empty())
        {
            Utility::Print("[ProgramSmokeTest] Build log:\n");
            Utility::Print(buildLog.c_str());
            Utility::Print("\n");
        }

        if (program == nullptr || !program->HasBytecode(ShaderStage::Compute))
        {
            ERROR("Program smoke test failed to build compute bytecode.");
            ASSERT(false);
            return;
        }

        ASSERT(!program->GetReflection().UsesDescriptorHeapIndexing());

        const ProgramBinding* constantsBinding = program->FindBinding("g_Material");
        ASSERT(constantsBinding != nullptr);
        ASSERT(constantsBinding->Kind == ProgramBindingKind::RootConstants);
        ASSERT(constantsBinding->Num32BitValues > 0);
        ASSERT(constantsBinding->RootIndex != UINT_MAX);

        const ProgramBinding* regularConstantsBinding = program->FindBinding("g_Frame");
        ASSERT(regularConstantsBinding != nullptr);
        ASSERT(regularConstantsBinding->Kind == ProgramBindingKind::ConstantBuffer);
        ASSERT(regularConstantsBinding->RootIndex != UINT_MAX);

        const ProgramParameter* directionParameter = program->FindParameter("g_Frame.Direction");
        ASSERT(directionParameter != nullptr);
        ASSERT(directionParameter->UniformSize == sizeof(float) * 3);

        const ProgramParameter* scalarAfterDirectionParameter =
            program->FindParameter("g_Frame.ScalarAfterDirection");
        ASSERT(scalarAfterDirectionParameter != nullptr);
        ASSERT(scalarAfterDirectionParameter->RelativeUniformOffset == sizeof(float) * 3);

        const ProgramParameter* basisParameter = program->FindParameter("g_Frame.Basis");
        ASSERT(basisParameter != nullptr);
        ASSERT(basisParameter->UniformSize == 44);
        ASSERT(basisParameter->UniformTypeStride == sizeof(float) * 12);
        ASSERT(basisParameter->MatrixLayout == ProgramMatrixLayout::ColumnMajor);
        ASSERT(basisParameter->MatrixVectorStride == sizeof(float) * 4);

        const ProgramBinding* outputBinding = program->FindBinding("g_Output");
        ASSERT(outputBinding != nullptr);
        ASSERT(outputBinding->Kind == ProgramBindingKind::RootBufferUAV);
        ASSERT(outputBinding->RootIndex != UINT_MAX);

        const ProgramBinding* inputBinding = program->FindBinding("g_Input");
        ASSERT(inputBinding != nullptr);
        ASSERT(inputBinding->Kind == ProgramBindingKind::RootBufferSRV);
        ASSERT(inputBinding->RootIndex != UINT_MAX);

        const ProgramBinding* inputArrayBinding = program->FindBinding("g_InputArray");
        ASSERT(inputArrayBinding != nullptr);
        ASSERT(inputArrayBinding->Kind == ProgramBindingKind::SRV);
        ASSERT(inputArrayBinding->Count == 2);

        const ProgramBinding* samplerBinding = program->FindBinding(
            "g_Material.TextureParams.StaticSampler");
        ASSERT(samplerBinding != nullptr);
        ASSERT(samplerBinding->Kind == ProgramBindingKind::StaticSampler);
        ASSERT(samplerBinding->RootIndex == UINT_MAX);

        const ProgramBinding* textureArrayBinding = program->FindBinding(
            "g_Material.TextureParams.Textures");
        ASSERT(textureArrayBinding != nullptr);
        ASSERT(textureArrayBinding->Kind == ProgramBindingKind::SRV);
        ASSERT(textureArrayBinding->Count == 2);

        const ProgramParameter* materialParameter = program->FindParameter("g_Material");
        ASSERT(materialParameter != nullptr);
        ASSERT(materialParameter->Kind == ProgramParameterKind::ParameterBlock);

        const ProgramParameter* baseValueParameter = program->FindParameter("g_Material.BaseValue");
        ASSERT(baseValueParameter != nullptr);
        ASSERT(baseValueParameter->Kind == ProgramParameterKind::Uniform);
        ASSERT(baseValueParameter->UniformBindingIndex != UINT_MAX);

        const ProgramParameter* textureArrayParameter = program->FindParameter(
            "g_Material.TextureParams.Textures");
        ASSERT(textureArrayParameter != nullptr);
        ASSERT(textureArrayParameter->Kind == ProgramParameterKind::Array);
        ASSERT(textureArrayParameter->ArrayCount == 2);

        const ProgramBinding* primaryLayerTextureBinding = program->FindBinding(
            "g_Material.Layers.PrimaryTexture");
        ASSERT(primaryLayerTextureBinding != nullptr);
        ASSERT(primaryLayerTextureBinding->Kind == ProgramBindingKind::SRV);
        ASSERT(primaryLayerTextureBinding->Count == 2);

        const ProgramParameter* primaryLayerTextureParameter = program->FindParameter(
            "g_Material.Layers.PrimaryTexture");
        ASSERT(primaryLayerTextureParameter != nullptr);
        ASSERT(primaryLayerTextureParameter->Kind == ProgramParameterKind::SRV);

        const ProgramBinding* secondaryLayerTextureBinding = program->FindBinding(
            "g_Material.Layers.SecondaryTexture");
        ASSERT(secondaryLayerTextureBinding != nullptr);
        ASSERT(secondaryLayerTextureBinding->Kind == ProgramBindingKind::SRV);
        ASSERT(secondaryLayerTextureBinding->Count == 2);

        const ProgramBinding* layerSamplerBinding = program->FindBinding(
            "g_Material.Layers.Sampler");
        ASSERT(layerSamplerBinding != nullptr);
        ASSERT(layerSamplerBinding->Kind == ProgramBindingKind::Sampler);
        ASSERT(layerSamplerBinding->Count == 2);

        const ProgramParameter* nestedInnerParameter = program->FindParameter(
            "g_Nested.Inner");
        ASSERT(nestedInnerParameter != nullptr);
        ASSERT(nestedInnerParameter->Kind == ProgramParameterKind::ParameterBlock);

        const ProgramBinding* nestedInnerConstants = program->FindBinding("g_Nested.Inner");
        ASSERT(nestedInnerConstants != nullptr);
        ASSERT(nestedInnerConstants->Kind == ProgramBindingKind::ConstantBuffer);

        ASSERT(program->GetRootSignature().GetSignature() != nullptr);

        g_ProgramSmokeTestPSO.SetRootSignature(program->GetRootSignature());
        g_ProgramSmokeTestPSO.SetComputeShader(program->GetD3D12Bytecode(ShaderStage::Compute));
        g_ProgramSmokeTestPSO.Finalize();

        ASSERT(g_ProgramSmokeTestPSO.GetPipelineStateObject() != nullptr);

        StructuredBuffer outputBuffer;
        outputBuffer.Create(L"Program Smoke Test Output", 1, sizeof(uint32_t));

        alignas(16) const uint32_t inputValue[4] = { 7, 0, 0, 0 };
        StructuredBuffer inputBuffer;
        inputBuffer.Create(L"Program Smoke Test Input", 1, sizeof(uint32_t), inputValue);

        ReadbackBuffer readbackBuffer;
        readbackBuffer.Create(L"Program Smoke Test Readback", 1, sizeof(uint32_t));

        ComputeContext& context = ComputeContext::Begin(L"Program Smoke Test");
        ProgramBinder binder(*program, context);
        binder.SetRootSignature();
        context.SetPipelineState(g_ProgramSmokeTestPSO);
        context.TransitionResource(inputBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(outputBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        binder["g_Material"]["BaseValue"].Set(40u);
        binder["g_Material"]["Layers"][0]["Bias"].Set(0u);
        binder["g_Material"]["Layers"][1]["Bias"].Set(2u);
        binder["g_Frame"]["Direction"].Set(Math::Vector3(1.0f, 2.0f, 3.0f));
        binder["g_Frame"]["ScalarAfterDirection"].Set(4.0f);
        binder["g_Frame"]["Offset"].Set(1u);
        binder["g_Frame"]["Basis"].Set(Math::Matrix3(
            Math::Vector3(1.0f, 2.0f, 3.0f),
            Math::Vector3(4.0f, 5.0f, 6.0f),
            Math::Vector3(7.0f, 8.0f, 9.0f)));
        binder["g_Frame"]["Enabled"].Set(true);
        binder["g_Nested"]["Inner"]["Value"].Set(3u);
        binder["g_Material"]["TextureParams"]["Textures"][0].SetSRV(
            GetDefaultTexture(kWhiteOpaque2D));
        binder["g_Material"]["TextureParams"]["Textures"][1].SetSRV(
            GetDefaultTexture(kBlackOpaque2D));
        binder["g_Material"]["TextureParams"]["DynamicSampler"].SetSampler(
            SamplerLinearClamp);
        for (uint32_t layerIndex = 0; layerIndex < 2; ++layerIndex)
        {
            binder["g_Material"]["Layers"][layerIndex]["PrimaryTexture"].SetSRV(
                GetDefaultTexture(kWhiteOpaque2D));
            binder["g_Material"]["Layers"][layerIndex]["SecondaryTexture"].SetSRV(
                GetDefaultTexture(kBlackOpaque2D));
            binder["g_Material"]["Layers"][layerIndex]["Sampler"].SetSampler(
                SamplerLinearClamp);
        }
        binder["g_Nested"]["Inner"]["Texture"].SetSRV(
            GetDefaultTexture(kWhiteOpaque2D));
        binder["g_Nested"]["OuterTexture"].SetSRV(
            GetDefaultTexture(kBlackOpaque2D));
        binder["g_InputArray"][0].SetSRV(inputBuffer.GetSRV());
        binder["g_InputArray"][1].SetSRV(inputBuffer.GetSRV());
        binder["g_Input"].SetRootBufferSRV(inputBuffer);
        binder.SetRootBufferUAV("g_Output", outputBuffer);
        binder.Apply();
        context.Dispatch(1, 1, 1);
        context.CopyBuffer(readbackBuffer, outputBuffer);
        context.Finish(true);

        const uint32_t* outputValue = static_cast<const uint32_t*>(readbackBuffer.Map());
        ASSERT(outputValue != nullptr);
        const uint32_t smokeTestResult = outputValue[0];
        ASSERT(smokeTestResult == 76);
        readbackBuffer.Unmap();

        Utility::Printf(
            "[ProgramSmokeTest] Reflected %llu bindings, built %llu bytes DXIL, dispatch result %u, Program version %llu\n",
            static_cast<unsigned long long>(program->GetReflection().GetBindingCount()),
            static_cast<unsigned long long>(program->GetBytecode(ShaderStage::Compute).Data.size()),
            smokeTestResult,
            static_cast<unsigned long long>(program->GetVersionId()));
    }
}

class Tests : public GameCore::IGameApp
{
public:

    Tests()
    {
    }

    virtual void Startup( void ) override;
    virtual void Cleanup( void ) override;

    virtual bool IsDone( void ) override;
    virtual void Update( float deltaT ) override;
    virtual void RenderScene( void ) override;

private:
};

CREATE_APPLICATION( Tests )

void Tests::Startup( void )
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    RunProgramRootSignatureLayoutTest();
    RunProgramRootSignatureLimitTests();
    RunProgramSmokeTest();
    RunProgramDescriptorHandleReflectionTest();
    RunProgramValuePackingTest();
    RunProgramGraphicsBinderTest();
    RunProgramDescriptorExecutionTest();
    RunProgramContextSwitchTest();
    RunProgramManagerCacheAndDiagnosticsTest();
    Utility::Print("[Tests] All tests passed.\n");
}

void Tests::Cleanup( void )
{
    // Free up resources in an orderly fashion
}

bool Tests::IsDone( void )
{
    return true;
}

void Tests::Update( float /*deltaT*/ )
{
    ScopedTimer _prof(L"Update State");

    // Update something
}

void Tests::RenderScene( void )
{
    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

    gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    gfxContext.ClearColor(g_SceneColorBuffer);
    gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV());
    gfxContext.SetViewportAndScissor(0, 0, g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());

    // Rendering something

    gfxContext.Finish();
}
