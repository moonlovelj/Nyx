#include "GeometryPageLayout.h"
#include <algorithm>
#include <bit>

namespace Renderer
{
    bool BuildRootGroupMask(std::span<const HierarchyNode> nodes, uint32_t groupCount,
        std::vector<uint8_t>& mask, std::string& error)
    {
        error.clear();
        mask.assign(groupCount, 0);
        for (const auto& node : nodes)
        {
            if (!node.Internal.IsGroup) continue;
            const uint32_t id = node.Leaf.GroupIndex;
            if (id >= groupCount)
            {
                error = "Hierarchy references invalid GroupId " + std::to_string(id);
                return false;
            }
            // Compare IEEE-754 bits: /fp:fast can treat NaN as equal to +infinity.
            if (std::bit_cast<uint32_t>(node.MaxParrentError) == 0x7f800000u) mask[id] = 1;
        }
        return true;
    }

    bool ValidateGeometryPageLayout(std::span<const GroupMetadata> groups,
        std::span<const PageMetadata> pages, std::span<const uint32_t> refs,
        std::span<const uint8_t> roots, uint32_t rootPageCount, uint32_t pageSize,
        uint64_t uncompressedSize, std::string& error)
    {
        error.clear();
        auto fail = [&](const std::string& message) { error = message; return false; };
        if (!pageSize || (pageSize & 3) || rootPageCount > pages.size() ||
            refs.size() != groups.size() || roots.size() != groups.size() ||
            uncompressedSize != uint64_t(pages.size()) * pageSize)
            return fail("Invalid geometry directory dimensions");
        std::vector<uint8_t> seen(groups.size(), 0);
        size_t nextRef = 0;
        for (size_t pageId = 0; pageId < pages.size(); ++pageId)
        {
            const auto& page = pages[pageId];
            if (!page.GroupCount || page.FirstGroupRef != nextRef || page.GroupCount > refs.size() - nextRef)
                return fail("Invalid reference range on page " + std::to_string(pageId));
            uint64_t end = 0;
            for (uint32_t i = 0; i < page.GroupCount; ++i)
            {
                const uint32_t id = refs[nextRef++];
                if (id >= groups.size() || seen[id]++)
                    return fail("Invalid or duplicate GroupId " + std::to_string(id));
                const auto& group = groups[id];
                // The writer emits members in offset order, allowing linear overlap validation.
                if (group.PageIndex != pageId || !group.SizeBytes || (group.SizeBytes & 3) ||
                    (group.OffsetInPage & 3) || group.OffsetInPage < end ||
                    uint64_t(group.OffsetInPage) + group.SizeBytes > pageSize ||
                    bool(roots[id]) != (pageId < rootPageCount))
                    return fail("Invalid placement/root class for GroupId " + std::to_string(id) +
                        " on page " + std::to_string(pageId));
                end = uint64_t(group.OffsetInPage) + group.SizeBytes;
            }
        }
        if (nextRef != groups.size()) return fail("Missing group references");
        return true;
    }

    bool BuildGeometryPageLayout(std::span<const GeometryPackingGroup> groups, uint32_t pageSize,
        GeometryPageLayout& layout, std::string& error)
    {
        layout = {};
        error.clear();
        if (!pageSize || (pageSize & 3) || groups.size() > UINT32_MAX)
        {
            error = "Invalid page size or group count";
            return false;
        }
        std::vector<uint8_t> roots(groups.size(), 0);
        layout.Groups.resize(groups.size());
        layout.PageGroupIndices.reserve(groups.size());
        for (const auto& group : groups)
        {
            // A nonzero size marks a GroupId already assigned below.
            if (group.GroupId >= groups.size() || layout.Groups[group.GroupId].SizeBytes != 0)
            {
                error = "Invalid or duplicate GroupId " + std::to_string(group.GroupId);
                return false;
            }
            if (!group.SizeBytes || (group.SizeBytes & 3) || group.SizeBytes > pageSize)
            {
                error = "GroupId " + std::to_string(group.GroupId) + " size " +
                    std::to_string(group.SizeBytes) + " invalid for page size " + std::to_string(pageSize);
                return false;
            }
            roots[group.GroupId] = group.IsRoot;
            layout.Groups[group.GroupId].SizeBytes = static_cast<uint32_t>(group.SizeBytes);
        }
        for (int pass = 0; pass != 2; ++pass)
        {
            uint32_t offset = pageSize; // Force a new page at each class boundary.
            for (uint32_t id = 0; id < groups.size(); ++id)
            {
                if (bool(roots[id]) != (pass == 0)) continue;
                auto& placement = layout.Groups[id];
                if (placement.SizeBytes > pageSize - offset)
                {
                    layout.Pages.push_back({static_cast<uint32_t>(layout.PageGroupIndices.size()), 0});
                    offset = 0;
                }
                placement.PageIndex = static_cast<uint32_t>(layout.Pages.size() - 1);
                placement.OffsetInPage = offset;
                placement.UncompressedSize = placement.SizeBytes;
                offset += placement.SizeBytes;
                ++layout.Pages.back().GroupCount;
                layout.PageGroupIndices.push_back(id);
            }
            if (pass == 0) layout.RootPageCount = static_cast<uint32_t>(layout.Pages.size());
        }
        layout.UncompressedSize = uint64_t(layout.Pages.size()) * pageSize;
#if defined(_DEBUG)
        return ValidateGeometryPageLayout(layout.Groups, layout.Pages, layout.PageGroupIndices,
            roots, layout.RootPageCount, pageSize, layout.UncompressedSize, error);
#else
        return true;
#endif
    }
}
