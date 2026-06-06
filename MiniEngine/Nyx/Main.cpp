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
#include "GpuBuffer.h"
#include "ProgramDesc.h"
#include "ProgramBinder.h"
#include "ProgramManager.h"
#include "ReadbackBuffer.h"
#include "Util/CommandLineArg.h"

using namespace GameCore;
using namespace Graphics;

namespace
{
    ComputePSO g_ProgramSmokeTestPSO(L"Program Smoke Test PSO");
    ComputePSO g_ProgramValuePackingTestPSO(L"Program Value Packing Test PSO");

    constexpr uint32_t kProgramValuePackingOutputCount = 128;

    std::string GetNyxSourcePath(const char* relativePath)
    {
        return Utility::GetBasePath(std::string(__FILE__)) + relativePath;
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

    void RunProgramDescriptorHandleReflectionTest()
    {
        ProgramDesc desc;
        desc.SetSourceFile(GetNyxSourcePath("Shaders\\ProgramDescriptorHandleTest.slang"))
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
        desc.SetSourceFile(GetNyxSourcePath("Shaders\\ProgramValuePackingTest.slang"))
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

        std::string smokeTestSource = GetNyxSourcePath("Shaders\\ProgramSmokeTest.slang");
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

class Nyx : public GameCore::IGameApp
{
public:

    Nyx()
    {
    }

    virtual void Startup( void ) override;
    virtual void Cleanup( void ) override;

    virtual bool IsDone( void ) override;
    virtual void Update( float deltaT ) override;
    virtual void RenderScene( void ) override;

private:
    bool m_SmokeTestOnly = false;
};

CREATE_APPLICATION( Nyx )

void Nyx::Startup( void )
{
    uint32_t smokeTestOnly = 0;
    CommandLineArgs::GetInteger(L"program-smoke-test-only", smokeTestOnly);
    m_SmokeTestOnly = smokeTestOnly != 0;

    RunProgramSmokeTest();
    RunProgramDescriptorHandleReflectionTest();
    RunProgramValuePackingTest();
}

void Nyx::Cleanup( void )
{
    // Free up resources in an orderly fashion
}

bool Nyx::IsDone( void )
{
    return m_SmokeTestOnly || IGameApp::IsDone();
}

void Nyx::Update( float /*deltaT*/ )
{
    ScopedTimer _prof(L"Update State");

    // Update something
}

void Nyx::RenderScene( void )
{
    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

    gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    gfxContext.ClearColor(g_SceneColorBuffer);
    gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV());
    gfxContext.SetViewportAndScissor(0, 0, g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());

    // Rendering something

    gfxContext.Finish();
}
