#include "GameCore.h"
#include "CameraController.h"
#include "BufferManager.h"
#include "Camera.h"
#include "CommandContext.h"
#include "TemporalEffects.h"
#include "MotionBlur.h"
#include "DepthOfField.h"
#include "PostEffects.h"
#include "SSAO.h"
#include "XeGTAO.h"
#include "FXAA.h"
#include "SystemTime.h"
#include "TextRenderer.h"
#include "ParticleEffectManager.h"
#include "GameInput.h"
#include "glTF.h"
#include "Renderer.h"
#include "VirtualShadowMap.h"
#include "Model.h"
#include "ModelLoader.h"
#include "ShadowCamera.h"
#include "Display.h"
#include "LightManager.h"
#include "IBL.h"
#include "TextureConvert.h"
#include "ModelInstanceManager.h"
#include "GeometryStreaming.h"
#include "imgui.h"
#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <wchar.h>

extern "C" {
	__declspec(dllexport) extern const UINT D3D12SDKVersion = 616; // Matches the Agility SDK version
	__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

using namespace GameCore;
using namespace Math;
using namespace Graphics;
using namespace std;

namespace
{
    Vector3 UpdateDirectionalShadowCamera(
        ShadowCamera& shadowCamera,
        const Camera& viewCamera,
        Vector3 lightDirection,
        float shadowDistance,
        uint32_t shadowWidth,
        uint32_t shadowHeight,
        uint32_t shadowBufferPrecision)
    {
        const float cameraNear = viewCamera.GetNearClip();
        const float cameraFar = viewCamera.GetFarClip();
        const float cameraDepthRange = std::max(cameraFar - cameraNear, 0.0001f);
        const float sliceFar = std::clamp(shadowDistance, cameraNear + 0.0001f, cameraFar);
        const float sliceFarT = (sliceFar - cameraNear) / cameraDepthRange;

        const Frustum& viewFrustum = viewCamera.GetWorldSpaceFrustum();
        std::array<Vector3, 8> receiverCorners;
        Vector3 receiverCenter(kZero);

        for (uint32_t cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
        {
            const Vector3 nearCorner = viewFrustum.GetFrustumCorner(
                static_cast<Frustum::CornerID>(cornerIndex));
            const Vector3 cameraFarCorner = viewFrustum.GetFrustumCorner(
                static_cast<Frustum::CornerID>(cornerIndex + 4));

            receiverCorners[cornerIndex] = nearCorner;
            receiverCorners[cornerIndex + 4] =
                Lerp(nearCorner, cameraFarCorner, sliceFarT);

            receiverCenter += receiverCorners[cornerIndex];
            receiverCenter += receiverCorners[cornerIndex + 4];
        }

        receiverCenter =
            receiverCenter * Scalar(1.0f / static_cast<float>(receiverCorners.size()));

        float receiverRadius = 0.0f;
        for (const Vector3& corner : receiverCorners)
        {
            receiverRadius = std::max(
                receiverRadius,
                static_cast<float>(Length(corner - receiverCenter)));
        }

        const float receiverPadding = std::max(receiverRadius * 0.01f, 0.01f);
        receiverRadius = std::max(receiverRadius + receiverPadding, 0.01f);

        lightDirection = Normalize(lightDirection);
        shadowCamera.SetLookDirection(lightDirection, Vector3(kZUnitVector));
        const Quaternion lightRotation = shadowCamera.GetRotation();
        const Quaternion inverseLightRotation = ~lightRotation;

        const Vector3 receiverCenterLS = inverseLightRotation * receiverCenter;
        const Vector3 sceneCenterLS =
            inverseLightRotation * ModelInstanceManager::GetInstanceDistributionCenter();
        const float sceneRadius = std::max(
            ModelInstanceManager::GetInstanceDistributionRadius(),
            receiverRadius);

        const float receiverMinZ =
            static_cast<float>(receiverCenterLS.GetZ()) - receiverRadius;
        const float receiverMaxZ =
            static_cast<float>(receiverCenterLS.GetZ()) + receiverRadius;
        const float casterMinZ =
            static_cast<float>(sceneCenterLS.GetZ()) - sceneRadius;
        const float casterMaxZ =
            static_cast<float>(sceneCenterLS.GetZ()) + sceneRadius;

        float minZ = std::min(receiverMinZ, casterMinZ);
        float maxZ = std::max(receiverMaxZ, casterMaxZ);
        const float depthPadding = std::max((maxZ - minZ) * 0.01f, 0.1f);
        minZ -= depthPadding;
        maxZ += depthPadding;

        const float shadowAspect =
            shadowHeight > 0
                ? static_cast<float>(shadowWidth) / static_cast<float>(shadowHeight)
                : 1.0f;
        float shadowWorldWidth = receiverRadius * 2.0f;
        float shadowWorldHeight = receiverRadius * 2.0f;
        if (shadowAspect > 1.0f)
            shadowWorldWidth *= shadowAspect;
        else if (shadowAspect > 0.0f)
            shadowWorldHeight /= shadowAspect;

        const Vector3 shadowCameraPositionLS(
            receiverCenterLS.GetX(),
            receiverCenterLS.GetY(),
            minZ);
        const Vector3 shadowCameraPosition =
            lightRotation * shadowCameraPositionLS;
        const Vector3 shadowBounds(
            shadowWorldWidth,
            shadowWorldHeight,
            std::max(maxZ - minZ, 0.01f));

        shadowCamera.UpdateMatrix(
            lightDirection,
            shadowCameraPosition,
            shadowBounds,
            shadowWidth,
            shadowHeight,
            shadowBufferPrecision);

        return receiverCenter;
    }
}

class SceneViewer : public GameCore::IGameApp
{
public:

    SceneViewer()
    {
    }

    virtual void Startup( void ) override;
    virtual void Cleanup( void ) override;

    virtual void Update( float deltaT ) override;
    virtual void RenderScene( void ) override;
    virtual void RenderUI( class GraphicsContext& ) override;
    void ReloadModel( const std::wstring& filePath, bool useOrbit, uint32_t instanceCount = 1 );
    void QueueModelReload( const std::wstring& filePath, bool useOrbit );

private:
	Camera m_PrevCamera;
	Camera m_Camera;
    unique_ptr<CameraController> m_CameraController;

    D3D12_VIEWPORT m_MainViewport;
    D3D12_RECT m_MainScissor;

    ShadowCamera m_SunShadowCamera;
    ShadowCamera m_PrevSunShadowCamera;
    float m_PreviousSunOrientation = 0.0f;
    float m_PreviousSunInclination = 0.0f;
    uint32_t m_SunVsmAddressGeneration = 0;
    bool m_HasSunVsmAddressGeneration = false;

    bool m_ShowLoadingUI = false;
    bool m_LoadPending = false;
    int m_LoadDelayFrames = 0;
    bool m_PendingUseOrbit = false;
    std::wstring m_PendingModelPath;
};

int wmain(int /*argc*/, wchar_t** /*argv*/)
{
    return GameCore::RunApplication(SceneViewer(), L"SceneViewer", GetModuleHandle(nullptr), SW_SHOWDEFAULT);
}

ExpVar g_SunLightIntensity("Viewer/Lighting/Sun Light Intensity", 6.0f, -5.0f, 20.0f, 0.1f); // unit: lx
NumVar g_SunOrientation("Viewer/Lighting/Sun Orientation", -0.5f, -100.0f, 100.0f, 0.1f );
NumVar g_SunInclination("Viewer/Lighting/Sun Inclination", 0.75f, 0.0f, 1.0f, 0.01f);
NumVar g_SunShadowDistance("Viewer/Lighting/Sun Shadow Distance", 100.0f, 1.0f, 100000.0f, 1.0f);
BoolVar g_RenderSunShadow("Viewer/Lighting/Render Sun Shadow", false);
//NumVar g_SunLightSize("Viewer/Lighting/Sun Light Size", 0.5f, 0.0f, 2.0f, 0.1f);
//NumVar g_SunShadowBias("Viewer/Lighting/Sun Shadow Bias", 4.f, 1.0f, 20.0f, 1.f );
//BoolVar g_SunShadow("Viewer/Lighting/Sun Shadow", false);
BoolVar g_UseglTFCamera("Camera/Use glTF Camera", false);

void ChangeIBLSet(EngineVar::ActionType);
void ChangeIBLBias(EngineVar::ActionType);
void ChangeGltfSet(EngineVar::ActionType);

//DynamicEnumVar g_IBLSet("Viewer/Lighting/Environment", ChangeIBLSet)ModelInstanceManager;
std::vector<std::pair<TextureRef, TextureRef>> g_IBLTextures;
//NumVar g_IBLBias("Viewer/Lighting/Gloss Reduction", 2.0f, 0.0f, 10.0f, 1.0f, ChangeIBLBias);

std::vector<TextureRef> g_IBLHDRITextures;
DynamicEnumVar g_IBLSet("Application/IBL", ChangeIBLSet);

DynamicEnumVar g_GltfSet("Application/GLTF", ChangeGltfSet);
std::vector<std::wstring> g_GltfFiles;
static bool g_ForceRebuild = false;
static SceneViewer* g_SceneViewer = nullptr;

void ChangeIBLSet(EngineVar::ActionType)
{
	int setIdx = g_IBLSet - 1;
    if (setIdx < 0)
    {
        IBL::ChangeIBL(nullptr);
    }
    else
    {
        auto IBLHDRITexture = g_IBLHDRITextures[setIdx];
        IBL::ChangeIBL(IBLHDRITexture);
    }

    Renderer::SetIBLTextures();
}

void ChangeIBLBias(EngineVar::ActionType)
{
    //Renderer::SetIBLBias(g_IBLBias);
}

void ChangeGltfSet(EngineVar::ActionType)
{
    if (g_SceneViewer == nullptr)
        return;

    int setIdx = g_GltfSet - 1;
    if (setIdx < 0)
    {
        g_SceneViewer->QueueModelReload(L"Assets/bunny/bunny.gltf", false);
        return;
    }

    if (setIdx >= (int)g_GltfFiles.size())
        return;

    g_SceneViewer->QueueModelReload(g_GltfFiles[setIdx], false);
}

namespace
{
    bool DirectoryExists(const std::filesystem::path& path)
    {
        std::error_code ec;
        return std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec);
    }

    bool IsSceneViewerResourceRoot(const std::filesystem::path& path)
    {
        return DirectoryExists(path / L"Assets") && DirectoryExists(path / L"Textures" / L"HDRIs");
    }

    void EnsureSceneViewerResourceRoot()
    {
        wchar_t cwdBuffer[MAX_PATH] = {};
        GetCurrentDirectoryW(_countof(cwdBuffer), cwdBuffer);
        std::filesystem::path cwd(cwdBuffer);
        if (IsSceneViewerResourceRoot(cwd))
        {
            Utility::Printf("Resource root: %ws\n", cwd.c_str());
            return;
        }

        wchar_t modulePath[MAX_PATH] = {};
        DWORD modulePathLen = GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
        if (modulePathLen == 0 || modulePathLen >= _countof(modulePath))
        {
            Utility::Printf("Warning: Unable to query executable path for resource root detection.\n");
            return;
        }

        const std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();
        std::vector<std::filesystem::path> candidates;

        for (std::filesystem::path probe = exeDir;;)
        {
            candidates.push_back(probe);
            candidates.push_back(probe / L"SceneViewer");
            candidates.push_back(probe / L"MiniEngine" / L"SceneViewer");

            std::filesystem::path parent = probe.parent_path();
            if (parent == probe)
                break;
            probe = parent;
        }

        for (const std::filesystem::path& candidate : candidates)
        {
            if (!IsSceneViewerResourceRoot(candidate))
                continue;

            std::error_code ec;
            std::filesystem::path normalized = std::filesystem::weakly_canonical(candidate, ec);
            const std::wstring target = ec ? candidate.wstring() : normalized.wstring();
            if (SetCurrentDirectoryW(target.c_str()))
            {
                Utility::Printf("Resource root: %ws\n", target.c_str());
            }
            else
            {
                Utility::Printf("Warning: Failed to switch working directory to %ws\n", target.c_str());
            }
            return;
        }

        Utility::Printf("Warning: SceneViewer resource root not found. cwd=%ws exeDir=%ws\n", cwd.c_str(), exeDir.c_str());
    }
}

void LoadIBLHDRITextures()
{
    Utility::Printf("Loading IBL hdri environment maps\n");

    g_IBLSet.AddEnum(L"None");

    WIN32_FIND_DATA ffd;
    HANDLE hFind = FindFirstFile(L"Textures/HDRIs/*.hdr", &ffd);

    if (hFind != INVALID_HANDLE_VALUE) do
    {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        std::wstring baseFile = ffd.cFileName;
		CompileTextureOnDemand(L"Textures/HDRIs/" + baseFile, TexConversionFlags::kQualityBC);
    }
    while (FindNextFile(hFind, &ffd) != 0);


    hFind = FindFirstFile(L"Textures/HDRIs/*.dds", &ffd);
	if (hFind != INVALID_HANDLE_VALUE) do
	{
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;

		std::wstring baseFile = ffd.cFileName;
        TextureRef hdriTex = TextureManager::LoadDDSFromFile(L"Textures/HDRIs/" + baseFile);
		if (hdriTex.IsValid())
		{
			g_IBLHDRITextures.push_back(hdriTex);
			g_IBLSet.AddEnum(baseFile);
		}
	} while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);

    if (g_IBLHDRITextures.size() > 0)
		g_IBLSet.Increment();

    Utility::Printf("Found %u IBL hdri environment map \n", g_IBLHDRITextures.size());
}

void CollectGltfFiles(const std::wstring& rootPath, const std::wstring& relativePath)
{
    std::wstring searchPath = rootPath;
    if (!relativePath.empty())
        searchPath += L"\\" + relativePath;
    searchPath += L"\\*";

    WIN32_FIND_DATA ffd;
    HANDLE hFind = FindFirstFile(searchPath.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (ffd.cFileName[0] == L'.')
            continue;

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            std::wstring subPath = relativePath.empty() ? ffd.cFileName : relativePath + L"\\" + ffd.cFileName;
            CollectGltfFiles(rootPath, subPath);
            continue;
        }

        const wchar_t* ext = wcsrchr(ffd.cFileName, L'.');
        if (ext == nullptr || _wcsicmp(ext, L".gltf") != 0)
            continue;

        std::wstring relPath = relativePath.empty() ? ffd.cFileName : relativePath + L"\\" + ffd.cFileName;
        std::wstring displayPath = relPath;
        std::replace(displayPath.begin(), displayPath.end(), L'\\', L'/');

        g_GltfFiles.push_back(rootPath + L"\\" + relPath);
        g_GltfSet.AddEnum(displayPath);
    } while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);
}

void LoadGltfFiles()
{
    Utility::Printf("Loading glTF files under Assets\n");

    g_GltfFiles.clear();
    g_GltfSet.AddEnum(L"Default");

    CollectGltfFiles(L"Assets", L"");

    Utility::Printf("Found %u glTF files under Assets\n", (uint32_t)g_GltfFiles.size());
}

void SceneViewer::ReloadModel(const std::wstring& filePath, bool useOrbit, uint32_t instanceCount)
{
    auto model = Renderer::LoadModel(filePath, g_ForceRebuild);
    if (model == nullptr)
    {
        Utility::Printf("Error: Failed to load model %ws\n", filePath.c_str());
        return;
    }

    if (ModelInstanceManager::GetNumModelInstances() > 0)
        ModelInstanceManager::Cleanup();

    ModelInstanceManager::Initialize(model, instanceCount);
    const Vector3 sceneCenter = ModelInstanceManager::GetInstanceDistributionCenter();
    const Vector3 sceneHalfExtents = ModelInstanceManager::GetInstanceDistributionHalfExtents();
    const float sceneRadius = std::max(ModelInstanceManager::GetInstanceDistributionRadius() * 0.1f, 0.01f);
    //const float lightExtent = std::max(ModelInstanceManager::GetInstanceDistributionRadius() * 0.25f, 5.0f);
    //const Vector3 lightHalfExtents = Max(sceneHalfExtents, Vector3(lightExtent));
    //Lighting::CreateRandomLights(sceneCenter - lightHalfExtents, sceneCenter + lightHalfExtents);

    const float halfX = std::max(static_cast<float>(sceneHalfExtents.GetX()), 5.f);
    const float halfZ = std::max(static_cast<float>(sceneHalfExtents.GetZ()), 5.f);
    const Vector3 cornerCenter = sceneCenter + Vector3(halfX, 0.0f, halfZ);
    const Vector3 eye = cornerCenter + std::max((float)model->m_BoundingSphere.GetRadius()* 0.4f, 5.f) * Normalize(Vector3(0.5f, 0.2f, 0.0f));
    m_Camera.SetEyeAtUp(eye, sceneCenter, Vector3(kYUnitVector));

    const float farClip = std::clamp(ModelInstanceManager::GetInstanceDistributionRadius(), 1000.0f, 100000.0f);
    m_Camera.SetZRange(0.01f, farClip);

    if (useOrbit)
    {
        m_CameraController.reset(new OrbitCamera(m_Camera, BoundingSphere(sceneCenter, sceneRadius), Vector3(kYUnitVector)));
    }
    else
    {
        FlyingFPSCamera* flyingCamera = new FlyingFPSCamera(m_Camera, Vector3(kYUnitVector));
        const float cameraMoveSpeed = std::max(sceneRadius * 0.2f, 0.1f);
        flyingCamera->SetMoveSpeed(cameraMoveSpeed);
        flyingCamera->SetStrafeSpeed(cameraMoveSpeed);
        m_CameraController.reset(flyingCamera);
    }
}

void SceneViewer::QueueModelReload( const std::wstring& filePath, bool useOrbit )
{
    m_PendingModelPath = filePath;
    m_PendingUseOrbit = useOrbit;
    m_LoadPending = true;
    m_LoadDelayFrames = 1;
    m_ShowLoadingUI = true;
}

void SceneViewer::Startup( void )
{
    // Setup your data

    EnsureSceneViewerResourceRoot();

    MotionBlur::Enable = true;
    TemporalEffects::EnableTAA = true;
	FXAA::Enable = false;
    PostEffects::EnableHDR = true;
    XeGTAO::Enable = true;
    PostEffects::BloomEnable = true;
    PostEffects::EnableAdaptation = true;

    g_SceneViewer = this;
    
    Renderer::Initialize();

    LoadIBLHDRITextures();

    if (g_IBLHDRITextures.size() > 0)
    {
        IBL::InitializeResources(g_IBLHDRITextures[0]);
    }
    else
    {
        IBL::InitializeResources(nullptr);
    }

    Renderer::SetIBLTextures();

    LoadGltfFiles();

    std::wstring gltfFileName;

    uint32_t rebuildValue;
    if (CommandLineArgs::GetInteger(L"rebuild", rebuildValue))
        g_ForceRebuild = rebuildValue != 0;
    else
        g_ForceRebuild = false;

    uint32_t instanceCount = 1;
    if (CommandLineArgs::GetInteger(L"instances", instanceCount))
        instanceCount = std::clamp(instanceCount, 1u, 1000000u);

    if (CommandLineArgs::GetString(L"model", gltfFileName))
        ReloadModel(gltfFileName, false, instanceCount);
    else
        ReloadModel(L"Assets/bunny/bunny.gltf", false, 10000);
}

void SceneViewer::Cleanup( void )
{
    // Free up resources in an orderly fashion
	ModelInstanceManager::Cleanup();

    g_IBLTextures.clear();

    g_IBLHDRITextures.clear();
    g_GltfFiles.clear();
    g_SceneViewer = nullptr;

    Renderer::Shutdown();
    Lighting::Shutdown();
    IBL::Shutdown();
}

namespace Graphics
{
    extern EnumVar DebugZoom;
}

void SceneViewer::Update( float deltaT )
{
    ScopedTimer _prof(L"Update State");

    if (GameInput::IsFirstPressed(GameInput::kLShoulder))
        DebugZoom.Decrement();
    else if (GameInput::IsFirstPressed(GameInput::kRShoulder))
        DebugZoom.Increment();

    if (m_LoadPending)
    {
        if (m_LoadDelayFrames > 0)
        {
            --m_LoadDelayFrames;
        }
        else
        {
            ReloadModel(m_PendingModelPath, m_PendingUseOrbit);
            m_LoadPending = false;
            m_ShowLoadingUI = false;
        }
    }

    const size_t NumCameras = ModelInstanceManager::GetModelInstance(0).GetNumCameras();
    const bool bUseglTFCamera = NumCameras > 0 && g_UseglTFCamera;

    if (!bUseglTFCamera)
        m_CameraController->Update(deltaT);

    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Update");

    ModelInstanceManager::Update(gfxContext, deltaT);

    gfxContext.Finish();
    
    //m_Camera.SetAspectRatio((float)g_DisplayHeight / g_DisplayWidth);
	if (bUseglTFCamera)
	{
		std::shared_ptr<Math::Camera> Camera = ModelInstanceManager::GetModelInstance(0).GetCameras()[0];
		m_Camera.SetFOV(Camera->GetFOV());
		m_Camera.SetZRange(Camera->GetNearClip(), Camera->GetFarClip());
		m_Camera.SetPosition(Camera->GetPosition());
		m_Camera.SetLookDirection(Camera->GetForwardVec(), Camera->GetUpVec());
		m_Camera.Update();
	}

    // We use viewport offsets to jitter sample positions from frame to frame (for TAA.)
    // D3D has a design quirk with fractional offsets such that the implicit scissor
    // region of a viewport is floor(TopLeftXY) and floor(TopLeftXY + WidthHeight), so
    // having a negative fractional top left, e.g. (-0.25, -0.25) would also shift the
    // BottomRight corner up by a whole integer.  One solution is to pad your viewport
    // dimensions with an extra pixel.  My solution is to only use positive fractional offsets,
    // but that means that the average sample position is +0.5, which I use when I disable
    // temporal AA.
    TemporalEffects::GetJitterOffset(m_MainViewport.TopLeftX, m_MainViewport.TopLeftY);

    m_MainViewport.Width = (float)g_SceneColorBuffer.GetWidth();
    m_MainViewport.Height = (float)g_SceneColorBuffer.GetHeight();
    m_MainViewport.MinDepth = 0.0f;
    m_MainViewport.MaxDepth = 1.0f;

    m_MainScissor.left = 0;
    m_MainScissor.top = 0;
    m_MainScissor.right = (LONG)g_SceneColorBuffer.GetWidth();
    m_MainScissor.bottom = (LONG)g_SceneColorBuffer.GetHeight();

	GeometryStreaming::Update(TemporalEffects::GetFrameIndex());
}

void SceneViewer::RenderScene( void )
{
    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render");

    uint32_t FrameIndex = TemporalEffects::GetFrameIndexMod2();
    const D3D12_VIEWPORT& viewport = m_MainViewport;
    const D3D12_RECT& scissor = m_MainScissor;

    ParticleEffectManager::Update(gfxContext.GetComputeContext(), Graphics::GetFrameTime());

    // gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    // gfxContext.ClearColor(g_SceneColorBuffer);
    // gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV());
    // gfxContext.SetViewportAndScissor(0, 0, g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());

    // Rendering something
    if (ModelInstanceManager::GetNumModelInstances() > 0)
    {
        IBL::Precompute(gfxContext);
        
         // Update global constants
        const float sunOrientation = g_SunOrientation;
        const float sunInclination = g_SunInclination;
        float costheta = cosf(sunOrientation);
        float sintheta = sinf(sunOrientation);
        float cosphi = cosf(sunInclination * 3.14159f * 0.5f);
        float sinphi = sinf(sunInclination * 3.14159f * 0.5f);

        Vector3 SunDirection = Normalize(Vector3( costheta * cosphi, sinphi, sintheta * cosphi ));
        const Vector3 sunShadowFocus = UpdateDirectionalShadowCamera(
            m_SunShadowCamera,
            m_Camera,
            -SunDirection,
            static_cast<float>(g_SunShadowDistance),
            g_ShadowBuffer.GetWidth(),
            g_ShadowBuffer.GetHeight(),
            32);

        uint32_t tileCountX = Math::DivideByMultiple(g_SceneColorBuffer.GetWidth(), Lighting::LightGridDim);
        uint32_t tileCountY = Math::DivideByMultiple(g_SceneColorBuffer.GetHeight(), Lighting::LightGridDim);

        Renderer::FrameConstants frameConstants;
        frameConstants.SunShadowMatrix = m_SunShadowCamera.GetShadowMatrix();
        Renderer::VirtualShadowMap::BeginFrame();

        Renderer::VirtualShadowMap::DirectionalVsmAddressDesc vsmAddressDesc;
        vsmAddressDesc.WorldToLightRotation = Matrix3(~m_SunShadowCamera.GetRotation());
        vsmAddressDesc.FocusPositionWS = sunShadowFocus;
        vsmAddressDesc.ViewProjMatrix = m_SunShadowCamera.GetViewProjMatrix();
        vsmAddressDesc.PrevViewProjMatrix = m_PrevSunShadowCamera.GetViewProjMatrix();
        if (!m_HasSunVsmAddressGeneration || sunOrientation != m_PreviousSunOrientation ||
            sunInclination != m_PreviousSunInclination)
        {
            ++m_SunVsmAddressGeneration;
            m_PreviousSunOrientation = sunOrientation;
            m_PreviousSunInclination = sunInclination;
            m_HasSunVsmAddressGeneration = true;
        }
        vsmAddressDesc.AddressGeneration = m_SunVsmAddressGeneration;
        frameConstants.SunVsmViewId = Renderer::VirtualShadowMap::AddDirectionalView(vsmAddressDesc);
        frameConstants.SunDirection = SunDirection;
        frameConstants.SunIntensity = Vector3(Scalar(g_SunLightIntensity));
        frameConstants.ShadowTexelSize = Vector4(
            1.0f / g_ShadowBuffer.GetWidth(),
            0.5f,
            4.0f,
            0.0f);
        frameConstants.InvTileDim = Vector4(
            1.0f / Lighting::LightGridDim,
            1.0f / Lighting::LightGridDim,
            0.0f,
            0.0f);
        frameConstants.TileCount[0] = tileCountX;
        frameConstants.TileCount[1] = tileCountY;
        frameConstants.TileCount[2] = 0;
        frameConstants.TileCount[3] = 0;
        frameConstants.FirstLightIndex[0] = Lighting::m_FirstConeLight;
        frameConstants.FirstLightIndex[1] = Lighting::m_FirstConeShadowedLight;
        frameConstants.FirstLightIndex[2] = 0;
        frameConstants.FirstLightIndex[3] = 0;
        frameConstants.FrameIndexMod2 = TemporalEffects::GetFrameIndexMod2();
        frameConstants.IBLLutTextureSize = IBL::g_IBLLutSize;
        frameConstants.IBLSpecularLDMapMipCount = Math::Log2(IBL::g_IBLSpecularLDMapSize) + 1;
        frameConstants.BindlessResourcesBaseIndex = Renderer::GetBindlessResourcesBaseOffset();

        {
            // Shadow
            ScopedTimer _outerprof(L"Sun Shadow", gfxContext);
            if (g_RenderSunShadow)
            {
                Renderer::RenderView shadowView;
                shadowView.SetCamera(m_SunShadowCamera);
                D3D12_VIEWPORT shadowViewport{};
                shadowViewport.Width = (float)g_ShadowBuffer.GetWidth();
                shadowViewport.Height = (float)g_ShadowBuffer.GetHeight();
                shadowViewport.MinDepth = 0.0f;
                shadowViewport.MaxDepth = 1.0f;
                shadowView.SetViewport(shadowViewport);
                D3D12_RECT shadowScissor;
                shadowScissor.left = 0;
                shadowScissor.top = 0;
                shadowScissor.right = (LONG)g_ShadowBuffer.GetWidth();
                shadowScissor.bottom = (LONG)g_ShadowBuffer.GetHeight();
                shadowView.SetScissor(shadowScissor);

                shadowView.SetPreviousCamera(m_PrevSunShadowCamera);
                shadowView.SetHZBSize(*Renderer::GetShadowHZBResources().Current);

                Renderer::HZBResources hzbResources = Renderer::GetShadowHZBResources();
                Renderer::RenderSceneDepth(gfxContext, shadowView, frameConstants, &hzbResources, g_ShadowBuffer);
            }
            else
            {
                gfxContext.TransitionResource(g_ShadowBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                gfxContext.ClearDepth(g_ShadowBuffer);
            }
        }

        // Begin rendering depth
        gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
        gfxContext.ClearDepth(g_SceneDepthBuffer);

        Renderer::RenderView mainView;
        mainView.SetCamera(m_Camera);
        mainView.SetPreviousCamera(m_PrevCamera);
        mainView.SetViewport(viewport);
        mainView.SetScissor(scissor);
        mainView.SetHZBSize(Renderer::GetCurrentHZB());


        {
            Renderer::UpdateGlobalDescriptors();

            Lighting::FillLightGrid(gfxContext, m_Camera);
            
            ScopedTimer _outerprof(L"Main Render", gfxContext);

            gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
            gfxContext.ClearColor(g_SceneColorBuffer);

            Renderer::RenderVisibility(gfxContext, mainView, frameConstants);

            Renderer::VirtualShadowMap::MarkRequestedPages(gfxContext, mainView);
            Renderer::VirtualShadowMap::AllocateRequestedPages(gfxContext);
            Renderer::VirtualShadowMap::BuildPhysicalPageViews(gfxContext);
            Renderer::VirtualShadowMap::ClearRequestedPhysicalPage(gfxContext, 0);

            Renderer::ResolveVBufferToGBuffer(gfxContext, mainView, frameConstants);

            XeGTAO::Render(gfxContext, m_Camera);

            Lighting::RenderDeferredLighting(gfxContext, mainView, frameConstants);

            Renderer::DrawSkybox(gfxContext, mainView, frameConstants);

        }
    }

    SSAO::LinearizeZ(gfxContext.GetComputeContext(), m_Camera, FrameIndex);

    // Some systems generate a per-pixel velocity buffer to better track dynamic and skinned meshes.  Everything
    // is static in our scene, so we generate velocity from camera motion and the depth buffer.  A velocity buffer
    // is necessary for all temporal effects (and motion blur).
    MotionBlur::GenerateCameraVelocityBuffer(gfxContext, m_Camera, true);

    TemporalEffects::ResolveImage(gfxContext);

    ParticleEffectManager::Render(gfxContext, m_Camera, g_SceneColorBuffer, g_SceneDepthBuffer,  g_LinearDepth[FrameIndex]);

    // Until I work out how to couple these two, it's "either-or".
    if (DepthOfField::Enable)
        DepthOfField::Render(gfxContext, m_Camera.GetNearClip(), m_Camera.GetFarClip());
    else
        MotionBlur::RenderObjectBlur(gfxContext, g_VelocityBuffer);

    m_PrevCamera = m_Camera;
    m_PrevSunShadowCamera = m_SunShadowCamera;
    gfxContext.Finish();
}

void SceneViewer::RenderUI( GraphicsContext& )
{
    if (!m_ShowLoadingUI)
        return;

    ImGuiIO& io = ImGui::GetIO();
    const char* text = "Loading model...";
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 padding(24.0f, 16.0f);
    const ImVec2 windowSize(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
    const ImVec2 windowPos((io.DisplaySize.x - windowSize.x) * 0.5f, (io.DisplaySize.y - windowSize.y) * 0.5f);

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(0, 0, 0, 120));

    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("Loading##SceneViewer", nullptr, flags))
    {
        ImGui::SetCursorPos(ImVec2(padding.x, padding.y));
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
}
