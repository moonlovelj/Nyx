//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author(s):  James Stanard
//             Chuck Walbourn (ATG)
//
// This code depends on DirectXTex
//

#include "TextureConvert.h"
#include "../Core/Utility.h"
#include <d3d11.h>
#include "DirectXTex.h"
#include <mutex>

#pragma comment(lib, "d3d11.lib")

using namespace DirectX;

#define GetFlag(f) ((Flags & f) != 0)

namespace
{
    DXGI_FORMAT GetOutputFormat(uint32_t flags, bool isHDR, size_t width, size_t height)
    {
        const bool compressed = (flags & (kDefaultBC | kQualityBC)) != 0 &&
            width % 4 == 0 && height % 4 == 0;
        if (isHDR)
        {
            if (compressed)
                return DXGI_FORMAT_BC6H_UF16;
            return (flags & kFloat32HDR) != 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        }

        const bool srgb = (flags & kSRGB) != 0;
        if (!compressed)
            return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        if ((flags & kQualityBC) != 0)
            return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        if ((flags & kPreserveAlpha) != 0)
            return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
        return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    }

    size_t GetFullMipCount(size_t width, size_t height)
    {
        size_t count = 1;
        for (size_t dim = std::max(width, height); dim > 1; dim >>= 1)
            ++count;
        return count;
    }

    HRESULT CompressTexture(const ScratchImage& source, DXGI_FORMAT format, ScratchImage& result)
    {
        if (format == DXGI_FORMAT_BC7_UNORM || format == DXGI_FORMAT_BC7_UNORM_SRGB ||
            format == DXGI_FORMAT_BC6H_UF16)
        {
            struct GpuEncoder
            {
                Microsoft::WRL::ComPtr<ID3D11Device> Device;
                std::mutex Mutex;
                GpuEncoder()
                {
                    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
                    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                        levels, _countof(levels), D3D11_SDK_VERSION, Device.GetAddressOf(), nullptr, nullptr);
                    Utility::Print(SUCCEEDED(hr) ? "Using GPU BC6H/BC7 texture compression.\n" :
                        "GPU texture encoder unavailable; using CPU compression.\n");
                }
            };
            static GpuEncoder encoder;
            // DirectXTex uses the device's immediate context, which must not be shared concurrently.
            std::lock_guard<std::mutex> lock(encoder.Mutex);
            if (encoder.Device)
            {
                const HRESULT hr = Compress(encoder.Device.Get(), source.GetImages(), source.GetImageCount(),
                    source.GetMetadata(), format, TEX_COMPRESS_DEFAULT, TEX_ALPHA_WEIGHT_DEFAULT, result);
                if (SUCCEEDED(hr))
                    return hr;
                Utility::Printf("GPU texture compression failed (%08X); falling back to CPU.\n", hr);
            }
        }
        return Compress(source.GetImages(), source.GetImageCount(), source.GetMetadata(),
            format, TEX_COMPRESS_DEFAULT, 0.5f, result);
    }
}

bool CompileTextureOnDemand(const std::wstring& originalFile, uint32_t flags)
{
    if (Utility::ToLower(Utility::GetFileExtension(originalFile)) == L"dds")
    {
        struct _stat64 sourceStat;
        return _wstat64(originalFile.c_str(), &sourceStat) == 0;
    }
    std::wstring ddsFile = Utility::RemoveExtension(originalFile) + L".dds";

    struct _stat64 ddsFileStat, srcFileStat;

    bool srcFileMissing = _wstat64(originalFile.c_str(), &srcFileStat) == -1;
    bool ddsFileMissing = _wstat64(ddsFile.c_str(), &ddsFileStat) == -1;

    if (srcFileMissing && ddsFileMissing)
    {
        Utility::Printf("Texture %ws is missing.\n", Utility::RemoveBasePath(originalFile).c_str());
        return false;
    }

    if (srcFileMissing)
        return !ddsFileMissing; // Packaged content may contain only the precompiled DDS.

    bool incompatible = false;
    if (!ddsFileMissing)
    {
        TexMetadata cachedMetadata = {};
        const std::wstring ext = Utility::ToLower(Utility::GetFileExtension(originalFile));
        const bool isHDR = ext == L"hdr" || ext == L"exr";
        incompatible = FAILED(GetMetadataFromDDSFile(ddsFile.c_str(), DDS_FLAGS_NONE, cachedMetadata)) ||
            cachedMetadata.format != GetOutputFormat(flags, isHDR, cachedMetadata.width, cachedMetadata.height) ||
            cachedMetadata.mipLevels != GetFullMipCount(cachedMetadata.width, cachedMetadata.height);
    }
    if (ddsFileMissing || ddsFileStat.st_mtime < srcFileStat.st_mtime || incompatible)
    {
        Utility::Printf("DDS texture %ws missing, stale, or format changed. Rebuilding.\n", Utility::RemoveBasePath(originalFile).c_str());
        return ConvertToDDS(originalFile, flags);
    }
    return true;
}

bool ConvertToDDS( const std::wstring& filePath, uint32_t Flags )
{
    bool bInterpretAsSRGB =	GetFlag(kSRGB);
    bool bPreserveAlpha =	GetFlag(kPreserveAlpha);
    bool bContainsNormals = GetFlag(kNormalMap);
    bool bBumpMap =			GetFlag(kBumpToNormal);
    bool bBlockCompress = GetFlag(kDefaultBC) || GetFlag(kQualityBC);
    bool bFlipImage =       GetFlag(kFlipVertical);
    bool bFloat32HDR = GetFlag(kFloat32HDR);

    // Can't be both
	ASSERT(!bInterpretAsSRGB || !bContainsNormals, "Texture cannot be both sRGB and NormalMap: %ws\n", filePath.c_str());
	ASSERT(!bPreserveAlpha || !bContainsNormals, "Texture cannot be both Alpha and NormalMap: %ws\n", filePath.c_str());

    Utility::Printf( "Converting file \"%ws\" to DDS.\n", filePath.c_str() );

    // Get extension as utf8 (ascii)
    std::wstring ext = Utility::ToLower(Utility::GetFileExtension(filePath));

    // Load texture image
    TexMetadata info;
    std::unique_ptr<ScratchImage> image(new ScratchImage);

    bool isHDR = false;
    if ( ext == L"dds" )
    {
        // TODO:  It might be desired to compress or recompress existing DDS files
        //Utility::Printf("Ignoring existing DDS \"%ws\".\n", filePath.c_str());
        //return false;

        HRESULT hr = LoadFromDDSFile( filePath.c_str(), DDS_FLAGS_NONE, &info, *image );
        if (FAILED(hr))
        {
            Utility::Printf( "Could not load texture \"%ws\" (DDS: %08X).\n", filePath.c_str(), hr );
            return false;
        }
    }
    else if ( ext == L"tga" )
    {
        HRESULT hr = LoadFromTGAFile( filePath.c_str(), &info, *image );
        if (FAILED(hr))
        {
            Utility::Printf( "Could not load texture \"%ws\" (TGA: %08X).\n", filePath.c_str(), hr );
            return false;
        }
    }
    else if ( ext == L"hdr" )
    {
        isHDR = true;
        HRESULT hr = LoadFromHDRFile( filePath.c_str(), &info, *image );
        if (FAILED(hr))
        {
            Utility::Printf( "Could not load texture \"%ws\" (HDR: %08X).\n", filePath.c_str(), hr );
            return false;
        }
    }
    else if ( ext == L"exr" )
    {
#ifdef USE_OPENEXR
        isHDR = true;
        HRESULT hr = LoadFromEXRFile(filePath.c_str(), &info, *image);
        if (FAILED(hr))
        {
            Utility::Printf("Could not load texture \"%ws\" (EXR: %08X).\n", filePath.c_str(), hr);
            return false;
        }
#else
        Utility::Printf("OpenEXR not supported for this build of the content exporter\n");
        return false;
#endif
    }
    else
    {
        // The material slot determines color space, regardless of PNG metadata.
        const WIC_FLAGS wicFlags = bInterpretAsSRGB ? WIC_FLAGS_FORCE_SRGB : WIC_FLAGS_IGNORE_SRGB;

        HRESULT hr = LoadFromWICFile( filePath.c_str(), wicFlags, &info, *image );
        if (FAILED(hr))
        {
            Utility::Printf( "Could not load texture \"%ws\" (WIC: %08X).\n", filePath.c_str(), hr );
            return false;
        }
    }

    if ( info.width > 16384 || info.height > 16384 )
    {
        Utility::Printf("Texture size (%Iu,%Iu) too large for feature level 11.0 or later (16384) \"%ws\".\n",
            info.width, info.height, filePath.c_str());
        return false;
    }

    if (bFlipImage)
    {
        std::unique_ptr<ScratchImage> timage(new ScratchImage);

        HRESULT hr = FlipRotate( image->GetImages()[0], TEX_FR_FLIP_VERTICAL, *timage );

        if (FAILED(hr))
        {
            Utility::Printf( "Could not flip image \"%ws\" (%08X).\n", filePath.c_str(), hr );
            return false;
        }
        else
        {
            image.swap(timage);
        }
       
    }

    DXGI_FORMAT tformat;
    const DXGI_FORMAT cformat = GetOutputFormat(Flags, isHDR, info.width, info.height);

    if (isHDR)
    {
        tformat = bFloat32HDR ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    }
    else
    {
        tformat = bInterpretAsSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    if (bBumpMap)
    {
        std::unique_ptr<ScratchImage> timage(new ScratchImage);

        HRESULT hr = ComputeNormalMap(image->GetImages(), image->GetImageCount(), image->GetMetadata(),
            CNMAP_CHANNEL_LUMINANCE, 10.0f, tformat, *timage);

        if (FAILED(hr))
        {
            Utility::Printf( "Could not compute normal map for \"%ws\" (%08X).\n", filePath.c_str(), hr );
            return false;
        }
        else
        {
            image.swap(timage);
            info.format = tformat;
        }
    }
    else if ( info.format != tformat )
    {
        std::unique_ptr<ScratchImage> timage(new ScratchImage);

        HRESULT hr = Convert( image->GetImages(), image->GetImageCount(), image->GetMetadata(),
            tformat, TEX_FILTER_DEFAULT, 0.5f, *timage );

        if (FAILED(hr))
        {
            Utility::Printf( "Could not convert \"%ws\" (%08X).\n", filePath.c_str(), hr );
            return false;
        }
        else
        {
            image.swap(timage);
            info.format = tformat;
        }
    }

    // Handle mipmaps
    if (info.mipLevels == 1)
    {
        std::unique_ptr<ScratchImage> timage(new ScratchImage);

        HRESULT hr = GenerateMipMaps( image->GetImages(), image->GetImageCount(), image->GetMetadata(), TEX_FILTER_DEFAULT, 0, *timage );

        if (FAILED(hr))
        {
            Utility::Printf( "Failing generating mimaps for \"%ws\" (WIC: %08X).\n", filePath.c_str(), hr );
            return false;
        }
        else
        {
            image.swap(timage);
        }
    }

    // Handle compression
    if (bBlockCompress)
    {
        if (info.width % 4 || info.height % 4)
        {
            Utility::Printf( "Texture size (%Iux%Iu) not a multiple of 4 \"%ws\", so skipping compress\n", info.width, info.height, filePath.c_str() );
        }
        else
        {
            std::unique_ptr<ScratchImage> timage(new ScratchImage);

            HRESULT hr = CompressTexture(*image, cformat, *timage);
            if (FAILED(hr))
            {
                Utility::Printf( "Failing compressing \"%ws\" (WIC: %08X).\n", filePath.c_str(), hr );
                return false;
            }
            else
            {
                image.swap(timage);
            }
        }
    }

    // Rename file extension to DDS
    const std::wstring wDest = Utility::RemoveExtension(filePath) + L".dds";
    const std::wstring temporaryFile = wDest + L".tmp." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetCurrentThreadId());
    
    // Save DDS
    HRESULT hr = SaveToDDSFile(image->GetImages(), image->GetImageCount(), image->GetMetadata(), DDS_FLAGS_NONE, temporaryFile.c_str());
    if( FAILED( hr ) )
    {
        Utility::Printf( "Could not write texture to file \"%ws\" (%08X).\n", wDest.c_str(), hr );
        DeleteFileW(temporaryFile.c_str());
        return false;
    }
    // Publish only complete output; a failed conversion leaves the previous cache intact.
    if (!MoveFileExW(temporaryFile.c_str(), wDest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        Utility::Printf("Could not replace DDS cache \"%ws\" (%u).\n", wDest.c_str(), GetLastError());
        DeleteFileW(temporaryFile.c_str());
        return false;
    }

    return true;
}
