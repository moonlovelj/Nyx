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
#include "Model.h"
#include "ModelLoader.h"
#include "ShadowCamera.h"
#include "Display.h"
#include "LightManager.h"
#include "IBL.h"
#include "TextureConvert.h"
#include "ModelInstanceManager.h"
#include "GeometryStreaming.h"
#include "Renderer.h"
#include "imgui.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
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
using Renderer::MeshSorter;

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
//NumVar g_SunLightSize("Viewer/Lighting/Sun Light Size", 0.5f, 0.0f, 2.0f, 0.1f);
//NumVar g_SunShadowBias("Viewer/Lighting/Sun Shadow Bias", 4.f, 1.0f, 20.0f, 1.f );
NumVar g_SunShadowCoverageDepth("Viewer/Lighting/Sun Shadow Coverage Depth", 1000.0f, 10.0f, 5000.0f, 5.0f);
NumVar g_SunShadowBoundsMin("Viewer/Lighting/Sun Shadow Bounds Min", 40.0f, 1.0f, 2000.0f, 1.0f);
NumVar g_SunShadowBoundsMax("Viewer/Lighting/Sun Shadow Bounds Max", 500.0f, 10.0f, 10000.0f, 5.0f);
BoolVar g_SunShadow("Viewer/Lighting/Sun Shadow", false);
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
        float costheta = cosf(g_SunOrientation);
        float sintheta = sinf(g_SunOrientation);
        float cosphi = cosf(g_SunInclination * 3.14159f * 0.5f);
        float sinphi = sinf(g_SunInclination * 3.14159f * 0.5f);

        Vector3 SunDirection = Normalize(Vector3( costheta * cosphi, sinphi, sintheta * cosphi ));

        const Vector3 lightDir = Normalize(-SunDirection); // Direction of light travel.
        const float sceneRadius = std::max(ModelInstanceManager::GetInstanceDistributionRadius(), 1.0f);
        const Vector3 cameraPos = m_Camera.GetPosition();
        const Vector3 cameraForward = Normalize(m_Camera.GetForwardVec());
        const Vector3 cameraRight = Normalize(m_Camera.GetRightVec());
        const Vector3 cameraUp = Normalize(m_Camera.GetUpVec());

        const float cameraAspect = m_MainViewport.Width / std::max(m_MainViewport.Height, 1.0f);
        const float nearDepth = std::max(m_Camera.GetNearClip(), 0.05f);
        const float coverDepth = std::max(
            nearDepth + 0.05f,
            std::min(m_Camera.GetFarClip(), (float)g_SunShadowCoverageDepth));
        const float tanHalfFov = tanf(0.5f * m_Camera.GetFOV());

        const float nearHalfHeight = nearDepth * tanHalfFov;
        const float nearHalfWidth = nearHalfHeight * cameraAspect;
        const float farHalfHeight = coverDepth * tanHalfFov;
        const float farHalfWidth = farHalfHeight * cameraAspect;

        const Vector3 nearCenter = cameraPos + cameraForward * nearDepth;
        const Vector3 farCenter = cameraPos + cameraForward * coverDepth;

        Vector3 frustumCorners[8] =
        {
            nearCenter + cameraRight * nearHalfWidth + cameraUp * nearHalfHeight,
            nearCenter + cameraRight * nearHalfWidth - cameraUp * nearHalfHeight,
            nearCenter - cameraRight * nearHalfWidth + cameraUp * nearHalfHeight,
            nearCenter - cameraRight * nearHalfWidth - cameraUp * nearHalfHeight,

            farCenter + cameraRight * farHalfWidth + cameraUp * farHalfHeight,
            farCenter + cameraRight * farHalfWidth - cameraUp * farHalfHeight,
            farCenter - cameraRight * farHalfWidth + cameraUp * farHalfHeight,
            farCenter - cameraRight * farHalfWidth - cameraUp * farHalfHeight
        };

        // Build a stable light-space basis for fitting camera-near coverage.
        Vector3 upRef(0.0f, 0.0f, 1.0f);
        if (std::abs((float)Dot(upRef, lightDir)) > 0.99f)
        {
            upRef = Vector3(0.0f, 1.0f, 0.0f);
        }
        const Vector3 lightAxisX = Normalize(Cross(upRef, lightDir));
        const Vector3 lightAxisY = Normalize(Cross(lightDir, lightAxisX));

        float minX = std::numeric_limits<float>::max();
        float maxX = -std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxY = -std::numeric_limits<float>::max();

        for (const Vector3& corner : frustumCorners)
        {
            const Vector3 rel = corner - cameraPos;
            const float x = (float)Dot(rel, lightAxisX);
            const float y = (float)Dot(rel, lightAxisY);
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }

        // Keep shadow focus around the near-eye camera frustum in light-space.
        const Vector3 shadowBoxCenter =
            cameraPos +
            lightAxisX * (0.5f * (minX + maxX)) +
            lightAxisY * (0.5f * (minY + maxY));

        const float frustumBoundsX = std::max(maxX - minX, 1.0f);
        const float frustumBoundsY = std::max(maxY - minY, 1.0f);
        const float nearCoverageBounds = 1.05f * std::max(frustumBoundsX, frustumBoundsY);

        const float minBoundsXY = (float)g_SunShadowBoundsMin;
        const float maxBoundsXY = std::max((float)g_SunShadowBoundsMax, minBoundsXY);
        const float boundsXY = std::clamp(nearCoverageBounds, minBoundsXY, maxBoundsXY);

        float boundsX = boundsXY;
        float boundsY = boundsXY;
        float boundsZ = std::max(2.0f * sceneRadius, 10.0f);

        const float shadowMapWidth = (float)g_ShadowBuffer.GetWidth();
        const float shadowMapHeight = (float)g_ShadowBuffer.GetHeight();

        // Enforce isotropic shadow texel density to avoid stretched shadow pixels.
        const float worldPerTexel = std::max(boundsX / shadowMapWidth, boundsY / shadowMapHeight);
        boundsX = worldPerTexel * shadowMapWidth;
        boundsY = worldPerTexel * shadowMapHeight;

        // ShadowCamera expects the center on the far plane of the represented shadow region.
        const Vector3 shadowCenter = shadowBoxCenter + lightDir * (0.5f * boundsZ);

        m_SunShadowCamera.UpdateMatrix(
            lightDir,
            shadowCenter,
            Vector3(boundsX, boundsY, boundsZ),
            (uint32_t)g_ShadowBuffer.GetWidth(),
            (uint32_t)g_ShadowBuffer.GetHeight(),
            32);

        GlobalConstants globals;
        globals.ViewProjMatrix = m_Camera.GetViewProjMatrix();
		globals.ProjMatrix = m_Camera.GetProjMatrix();
		globals.InverseViewProjMatrix = Invert(m_Camera.GetViewProjMatrix());
        globals.SunShadowMatrix = m_SunShadowCamera.GetShadowMatrix();
        globals.ViewerPos = m_Camera.GetPosition();
        globals.SunDirection = SunDirection;
        globals.SunIntensity = Vector3(Scalar(g_SunLightIntensity));

		globals.PrevViewMatrix = m_PrevCamera.GetViewMatrix();
		globals.PrevViewProjMatrix = m_PrevCamera.GetViewProjMatrix();
		globals.PrevProjMatrix = m_PrevCamera.GetProjMatrix();
		globals.PrevViewerPos = m_PrevCamera.GetPosition();
		globals.HZBSizeAndInv[0] = (float)Renderer::GetCurrentHZB().GetWidth();
		globals.HZBSizeAndInv[1] = (float)Renderer::GetCurrentHZB().GetHeight();
		globals.HZBSizeAndInv[2] = 1.f / (float)Renderer::GetCurrentHZB().GetWidth();
		globals.HZBSizeAndInv[3] = 1.f / (float)Renderer::GetCurrentHZB().GetHeight();


        globals.ShadowTexelSize[0] = 1.0f / g_ShadowBuffer.GetWidth();
        //globals.ShadowTexelSize[1] = g_SunLightSize;
        //globals.ShadowTexelSize[2] = g_SunShadowBias;
        globals.ShadowTexelSize[1] = 0.5f;
        globals.ShadowTexelSize[2] = 4.f;

        globals.InvTileDim[0] = 1.0f / Lighting::LightGridDim;
        globals.InvTileDim[1] = 1.0f / Lighting::LightGridDim;
        uint32_t tileCountX = Math::DivideByMultiple(g_SceneColorBuffer.GetWidth(), Lighting::LightGridDim);
        uint32_t tileCountY = Math::DivideByMultiple(g_SceneColorBuffer.GetHeight(), Lighting::LightGridDim);
        globals.TileCount[0] = tileCountX;
        globals.TileCount[1] = tileCountY;
        globals.FirstLightIndex[0] = Lighting::m_FirstConeLight;
        globals.FirstLightIndex[1] = Lighting::m_FirstConeShadowedLight;

        globals.ViewportWidth = g_SceneColorBuffer.GetWidth();
        globals.ViewportHeight = g_SceneColorBuffer.GetHeight();
		globals.InvViewportWidth = 1.0f / (float)g_SceneColorBuffer.GetWidth();
		globals.InvViewportHeight = 1.0f / (float)g_SceneColorBuffer.GetHeight();

        globals.FrameIndexMod2 = TemporalEffects::GetFrameIndexMod2();
        globals.IBLLutTextureSize = IBL::g_IBLLutSize;
        globals.IBLSpecularLDMapMipCount = Math::Log2(IBL::g_IBLSpecularLDMapSize) + 1;

		globals.BindlessResourcesBaseIndex = Renderer::GetBindlessResourcesBaseOffset();

        if (!Renderer::FreezeCull)
        {
            gfxContext.TransitionResource(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gfxContext.ClearUAV(GeometryStreaming::m_GeometryStreamingRequestMaskGPU, 0);
        }
        // Begin rendering depth
        gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
        gfxContext.ClearDepth(g_SceneDepthBuffer);

        MeshSorter shadowSorter(Renderer::kShadows);
        shadowSorter.SetCamera(m_SunShadowCamera);
        shadowSorter.SetDepthStencilTarget(g_ShadowBuffer);
        {
            globals.ViewportWidth = g_ShadowBuffer.GetWidth();
            globals.ViewportHeight = g_ShadowBuffer.GetHeight();
            globals.InvViewportWidth = 1.0f / (float)g_ShadowBuffer.GetWidth();
            globals.InvViewportHeight = 1.0f / (float)g_ShadowBuffer.GetHeight();

            ScopedTimer _outerprof(L"Sun Shadow", gfxContext);
            shadowSorter.RenderMeshes(Renderer::kZPass, gfxContext, globals);

            globals.ViewportWidth = g_SceneColorBuffer.GetWidth();
            globals.ViewportHeight = g_SceneColorBuffer.GetHeight();
            globals.InvViewportWidth = 1.0f / (float)g_SceneColorBuffer.GetWidth();
            globals.InvViewportHeight = 1.0f / (float)g_SceneColorBuffer.GetHeight();

        }

        MeshSorter sorter(Renderer::kDefault);
		sorter.SetCamera(m_Camera);
		sorter.SetViewport(viewport);
		sorter.SetScissor(scissor);
		sorter.SetDepthStencilTarget(g_SceneDepthBuffer);
		sorter.AddRenderTarget(g_SceneColorBuffer);
        sorter.Sort();

        {
            Lighting::FillLightGrid(gfxContext, m_Camera);
            
            ScopedTimer _outerprof(L"Main Render", gfxContext);

            gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
            gfxContext.ClearColor(g_SceneColorBuffer);
            
            {
                ScopedTimer _prof(L"Render VBuffer", gfxContext);
				gfxContext.TransitionResource(g_VisibilityBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
                gfxContext.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
				gfxContext.ClearColor(g_VisibilityBuffer);
                sorter.RenderMeshes(Renderer::kVBuffer, gfxContext, globals);
            }

            {
				gfxContext.TransitionResource(g_GBufferA, D3D12_RESOURCE_STATE_RENDER_TARGET);
				gfxContext.TransitionResource(g_GBufferB, D3D12_RESOURCE_STATE_RENDER_TARGET);
				gfxContext.TransitionResource(g_GBufferC, D3D12_RESOURCE_STATE_RENDER_TARGET);
				gfxContext.TransitionResource(g_GBufferD, D3D12_RESOURCE_STATE_RENDER_TARGET);
				gfxContext.ClearColor(g_GBufferA);
				gfxContext.ClearColor(g_GBufferB);
				gfxContext.ClearColor(g_GBufferC);
				gfxContext.ClearColor(g_GBufferD);

				Renderer::ResolveVBufferToGBuffer(gfxContext, globals);
            }

            XeGTAO::Render(gfxContext, m_Camera);

            Lighting::RenderDeferredLighting(gfxContext, globals);

            Renderer::DrawSkybox(gfxContext, m_Camera, viewport, scissor);

            {
                ScopedTimer _prof(L"Draw Transparent", gfxContext);
                sorter.RenderMeshes(Renderer::kTransparent, gfxContext, globals);
            }
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
