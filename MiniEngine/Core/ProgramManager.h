//
// Copyright (c) Moon. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include "pch.h"
#include "Program.h"

#include <mutex>
#include <unordered_map>

class ProgramManager
{
public:
    static ProgramManager& Get();

    std::shared_ptr<Program> GetProgram(const ProgramDesc& desc, std::string* outBuildLog = nullptr);
    void ClearCache();

private:
    ProgramManager() = default;

    std::shared_ptr<Program> BuildProgram(const ProgramDesc& desc, std::string& buildLog);
    bool EnsureGlobalSession(std::string& buildLog);

    std::mutex m_Mutex;
    uint64_t m_NextVersionId = 1;
    std::unordered_map<std::string, std::shared_ptr<Program>> m_Cache;

    struct SessionState;
    std::unique_ptr<SessionState> m_SessionState;
};
