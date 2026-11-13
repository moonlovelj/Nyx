#pragma once

// Shared C++/Slang request layout: sixteen 2-bit group masks per uint.
#define GEOMETRY_REQUEST_SHADOW 1u
#define GEOMETRY_REQUEST_MAIN 2u
#define GEOMETRY_REQUEST_MASK 3u
#define GEOMETRY_REQUEST_GROUPS_PER_WORD 16u
