#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "MeshletStructs.h"

namespace Renderer
{
    struct GeometryPackingGroup
    {
        uint32_t GroupId;
        uint64_t SizeBytes;
        bool IsRoot;
    };

    struct GeometryPageLayout
    {
        std::vector<GroupMetadata> Groups;
        std::vector<PageMetadata> Pages;
        std::vector<uint32_t> PageGroupIndices;
        uint32_t RootPageCount = 0;
        uint64_t UncompressedSize = 0;
    };

    bool BuildRootGroupMask(std::span<const HierarchyNode> nodes, uint32_t groupCount,
        std::vector<uint8_t>& mask, std::string& error);
    bool BuildGeometryPageLayout(std::span<const GeometryPackingGroup> groups, uint32_t pageSize,
        GeometryPageLayout& layout, std::string& error);
    bool ValidateGeometryPageLayout(std::span<const GroupMetadata> groups,
        std::span<const PageMetadata> pages, std::span<const uint32_t> refs,
        std::span<const uint8_t> roots, uint32_t rootPageCount, uint32_t pageSize,
        uint64_t uncompressedSize, std::string& error);
}
