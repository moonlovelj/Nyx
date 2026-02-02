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
// Author:  James Stanard 
//

#include "pch.h"
#include "SystemTime.h"
#include "Display.h"
#include "GraphRenderer.h"
#include "GameInput.h"
#include "GpuTimeManager.h"
#include "CommandContext.h"
#include "ImGuiManager.h"
#include "imgui.h"
#include <vector>
#include <unordered_map>
#include <array>
#include <algorithm>

using namespace Graphics;
using namespace GraphRenderer;
using namespace Math;
using namespace std;

#define PERF_GRAPH_ERROR uint32_t(0xFFFFFFFF)
namespace EngineProfiling
{
    bool Paused = false;
}

class StatHistory
{
public:
    StatHistory()
    {
        for (uint32_t i = 0; i < kHistorySize; ++i)
            m_RecentHistory[i] = 0.0f;
        for (uint32_t i = 0; i < kExtendedHistorySize; ++i)
            m_ExtendedHistory[i] = 0.0f;
        m_Average = 0.0f;
        m_Minimum = 0.0f;
        m_Maximum = 0.0f;
    }

    void RecordStat( uint32_t FrameIndex, float Value )
    {
        m_RecentHistory[FrameIndex % kHistorySize] = Value;
        m_ExtendedHistory[FrameIndex % kExtendedHistorySize] = Value;
        m_Recent = Value;

        uint32_t ValidCount = 0;
        m_Minimum = FLT_MAX;
        m_Maximum = 0.0f;
        m_Average = 0.0f;

        for (float val : m_RecentHistory)
        {
            if (val > 0.0f)
            {
                ++ValidCount;
                m_Average += val;
                m_Minimum = min(val, m_Minimum);
                m_Maximum = max(val, m_Maximum);
            }
        }

        if (ValidCount > 0)
            m_Average /= (float)ValidCount;
        else
            m_Minimum = 0.0f;
    }

    float GetLast(void) const { return m_Recent; }
    float GetMax(void) const { return m_Maximum; }
    float GetMin(void) const { return m_Minimum; }
    float GetAvg(void) const { return m_Average; }

    const float* GetHistory(void) const { return m_ExtendedHistory; }
    uint32_t GetHistoryLength(void) const { return kExtendedHistorySize; }

private:
    static const uint32_t kHistorySize = 64;
    static const uint32_t kExtendedHistorySize = 256;
    float m_RecentHistory[kHistorySize];
    float m_ExtendedHistory[kExtendedHistorySize];
    float m_Recent;
    float m_Average;
    float m_Minimum;
    float m_Maximum;
};

class StatPlot
{
public:
    StatPlot(StatHistory& Data, Color Col = Color(1.0f, 1.0f, 1.0f))
        : m_StatData(Data), m_PlotColor(Col)
    {
    }

    void SetColor( Color Col )
    {
        m_PlotColor = Col;
    }

private:
    StatHistory& m_StatData;
    Color m_PlotColor;
};

class StatGraph
{
public:
    StatGraph(const wstring& Label, D3D12_RECT Window)
        : m_Label(Label), m_Window(Window), m_BGColor(0.0f, 0.0f, 0.0f, 0.2f)
    {
    }

    void SetLabel(const wstring& Label)
    {
        m_Label = Label;
    }

    void SetWindow(D3D12_RECT Window)
    {
        m_Window = Window;
    }

    uint32_t AddPlot( const StatPlot& P )
    {
        uint32_t Idx = (uint32_t)m_Stats.size();
        m_Stats.push_back(P);
        return Idx;
    }

    StatPlot& GetPlot( uint32_t Handle );

    void Draw( GraphicsContext& Context );

private:
    wstring m_Label;
    D3D12_RECT m_Window;
    vector<StatPlot> m_Stats;
    Color m_BGColor;
    float m_PeakValue;
};

class GraphManager
{
public:

private:
    vector<StatGraph> m_Graphs;
};

class GpuTimer
{
public:

    GpuTimer()
    {
        m_TimerIndex = GpuTimeManager::NewTimer();
    }

    void Start(CommandContext& Context)
    {
        GpuTimeManager::StartTimer(Context, m_TimerIndex);
    }

    void Stop(CommandContext& Context)
    {
        GpuTimeManager::StopTimer(Context, m_TimerIndex);
    }

    float GetTime(void)
    {
        return GpuTimeManager::GetTime(m_TimerIndex);
    }

    uint32_t GetTimerIndex(void)
    {
        return m_TimerIndex;
    }
private:

    uint32_t m_TimerIndex;
};

class NestedTimingTree
{
public:
    NestedTimingTree( const wstring& name, NestedTimingTree* parent = nullptr )
        : m_Name(name), m_Parent(parent), m_IsExpanded(false), m_IsGraphed(false), m_GraphHandle(PERF_GRAPH_ERROR) {}

    NestedTimingTree* GetChild( const wstring& name )
    {
        auto iter = m_LUT.find(name);
        if (iter != m_LUT.end())
            return iter->second;

        NestedTimingTree* node = new NestedTimingTree(name, this);
        m_Children.push_back(node);
        m_LUT[name] = node;
        return node;
    }

    NestedTimingTree* NextScope( void )
    {
        if (m_IsExpanded && m_Children.size() > 0)
            return m_Children[0];

        return m_Parent->NextChild(this);
    }

    NestedTimingTree* PrevScope( void )
    {
        NestedTimingTree* prev = m_Parent->PrevChild(this);
        return prev == m_Parent ? prev : prev->LastChild();
    }

    NestedTimingTree* FirstChild( void )
    {
        return m_Children.size() == 0 ? nullptr : m_Children[0];
    }

    NestedTimingTree* LastChild( void )
    {
        if (!m_IsExpanded || m_Children.size() == 0)
            return this;

        return m_Children.back()->LastChild();
    }

    NestedTimingTree* NextChild( NestedTimingTree* curChild )
    {
        ASSERT(curChild->m_Parent == this);

        for (auto iter = m_Children.begin(); iter != m_Children.end(); ++iter)
        {
            if (*iter == curChild)
            {
                auto nextChild = iter; ++nextChild;
                if (nextChild != m_Children.end())
                    return *nextChild;
            }
        }

        if (m_Parent != nullptr)
            return m_Parent->NextChild(this);
        else
            return &sm_RootScope;
    }

    NestedTimingTree* PrevChild( NestedTimingTree* curChild )
    {
        ASSERT(curChild->m_Parent == this);

        if (*m_Children.begin() == curChild)
        {
            if (this == &sm_RootScope)
                return sm_RootScope.LastChild();
            else
                return this;
        }

        for (auto iter = m_Children.begin(); iter != m_Children.end(); ++iter)
        {
            if (*iter == curChild)
            {
                auto prevChild = iter; --prevChild;
                return *prevChild;
            }
        }

        ERROR("All attempts to find a previous timing sample failed");
        return nullptr;
    }

    void StartTiming( CommandContext* Context )
    {
        m_StartTick = SystemTime::GetCurrentTick();
        if (Context == nullptr)
            return;

        m_GpuTimer.Start(*Context);

        Context->PIXBeginEvent(m_Name.c_str());
    }

    void StopTiming( CommandContext* Context )
    {
        m_EndTick = SystemTime::GetCurrentTick();
        if (Context == nullptr)
            return;

        m_GpuTimer.Stop(*Context);

        Context->PIXEndEvent();
    }

    void GatherTimes(uint32_t FrameIndex)
    {
        if (EngineProfiling::Paused)
        {
            for (auto node : m_Children)
                node->GatherTimes(FrameIndex);
            return;
        }
        m_CpuTime.RecordStat(FrameIndex, 1000.0f * (float)SystemTime::TimeBetweenTicks(m_StartTick, m_EndTick));
        m_GpuTime.RecordStat(FrameIndex, 1000.0f * m_GpuTimer.GetTime());

        for (auto node : m_Children)
            node->GatherTimes(FrameIndex);

        m_StartTick = 0;
        m_EndTick = 0;
    }

    void SumInclusiveTimes(float& cpuTime, float& gpuTime)
    {
        cpuTime = 0.0f;
        gpuTime = 0.0f;
        for (auto iter = m_Children.begin(); iter != m_Children.end(); ++iter)
        {
            cpuTime += (*iter)->m_CpuTime.GetLast();
            gpuTime += (*iter)->m_GpuTime.GetLast();
        }
    }

    static void PushProfilingMarker( const wstring& name, CommandContext* Context );
    static void PopProfilingMarker( CommandContext* Context );
    static void Update( void );
    static void UpdateTimes( void )
    {
        uint32_t FrameIndex = (uint32_t)Graphics::GetFrameCount();

        GpuTimeManager::BeginReadBack();
        sm_RootScope.GatherTimes(FrameIndex);
        s_FrameDelta.RecordStat(FrameIndex, GpuTimeManager::GetTime(0));
        GpuTimeManager::EndReadBack();

        float TotalCpuTime, TotalGpuTime;
        sm_RootScope.SumInclusiveTimes(TotalCpuTime, TotalGpuTime);
        s_TotalCpuTime.RecordStat(FrameIndex, TotalCpuTime);
        s_TotalGpuTime.RecordStat(FrameIndex, TotalGpuTime);

    }

    static float GetTotalCpuTime(void) { return s_TotalCpuTime.GetAvg(); }
    static float GetTotalGpuTime(void) { return s_TotalGpuTime.GetAvg(); }
    static float GetFrameDelta(void) { return s_FrameDelta.GetAvg(); }

    static const StatHistory& GetTotalCpuHistory(void) { return s_TotalCpuTime; }
    static const StatHistory& GetTotalGpuHistory(void) { return s_TotalGpuTime; }
    static const StatHistory& GetFrameDeltaHistory(void) { return s_FrameDelta; }

    static void RenderImGuiTree( void )
    {
        sm_RootScope.RenderImGuiRow(0);
    }

    void Toggle()
    { 
        //if (m_GraphHandle == PERF_GRAPH_ERROR)
        //    m_GraphHandle = GraphRenderer::InitGraph(GraphType::Profile);
        //m_IsGraphed = GraphRenderer::ManageGraphs(m_GraphHandle, GraphType::Profile);
    }
    bool IsGraphed(){ return m_IsGraphed;}

private:

    void RenderImGuiRow( int depth );
    void DeleteChildren( void )
    {
        for (auto node : m_Children)
            delete node;
        m_Children.clear();
    }

    wstring m_Name;
    NestedTimingTree* m_Parent;
    vector<NestedTimingTree*> m_Children;
    unordered_map<wstring, NestedTimingTree*> m_LUT;
    int64_t m_StartTick;
    int64_t m_EndTick;
    StatHistory m_CpuTime;
    StatHistory m_GpuTime;
    bool m_IsExpanded;
    GpuTimer m_GpuTimer;
    bool m_IsGraphed;
    GraphHandle m_GraphHandle;
    static StatHistory s_TotalCpuTime;
    static StatHistory s_TotalGpuTime;
    static StatHistory s_FrameDelta;
    static NestedTimingTree sm_RootScope;
    static NestedTimingTree* sm_CurrentNode;

};

StatHistory NestedTimingTree::s_TotalCpuTime;
StatHistory NestedTimingTree::s_TotalGpuTime;
StatHistory NestedTimingTree::s_FrameDelta;
NestedTimingTree NestedTimingTree::sm_RootScope(L"");
NestedTimingTree* NestedTimingTree::sm_CurrentNode = &NestedTimingTree::sm_RootScope;
namespace EngineProfiling
{
    BoolVar DrawFrameRate("Display Frame Rate", true);
    BoolVar DrawProfiler("Display Profiler", false);
    BoolVar DrawPerfGraph("Display Performance Graph", false);
    //const bool DrawPerfGraph = true;
    
    void Update( void )
    {
        if (!ImGuiManager::WantsCaptureKeyboard()
            && (GameInput::IsFirstPressed( GameInput::kStartButton )
            || GameInput::IsFirstPressed( GameInput::kKey_space )))
        {
            Paused = !Paused;
        }
        NestedTimingTree::UpdateTimes();
    }

    void BeginBlock(const wstring& name, CommandContext* Context)
    {
        NestedTimingTree::PushProfilingMarker(name, Context);
    }

    void EndBlock(CommandContext* Context)
    {
        NestedTimingTree::PopProfilingMarker(Context);
    }

    bool IsPaused()
    {
        return Paused;
    }

    void RenderImGui()
    {
        ImGuiIO& io = ImGui::GetIO();
        const float margin = 10.0f;
        const float minWidth = 320.0f;
        const float maxWidth = 480.0f;
        const float rightWidth = std::min(maxWidth, std::max(minWidth, io.DisplaySize.x * 0.32f));
        const float graphHeight = std::min(180.0f, std::max(120.0f, io.DisplaySize.y * 0.18f));
        float rightX = io.DisplaySize.x - rightWidth - margin;
        float rightY = margin;

        if (DrawFrameRate)
        {
            float cpuTime = NestedTimingTree::GetTotalCpuTime();
            float gpuTime = NestedTimingTree::GetTotalGpuTime();
            float frameRate = 1.0f / NestedTimingTree::GetFrameDelta();

            ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

            if (ImGui::Begin("Frame Rate", nullptr, flags))
                ImGui::Text("CPU %.3f ms | GPU %.3f ms | %u Hz", cpuTime, gpuTime, (uint32_t)(frameRate + 0.5f));
            ImGui::End();
        }

        if (DrawPerfGraph)
        {
            bool open = DrawPerfGraph;
            ImGui::SetNextWindowPos(ImVec2(rightX, rightY), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(rightWidth, graphHeight), ImGuiCond_FirstUseEver);
            ImGuiWindowFlags flags = 0;
            if (ImGui::Begin("Performance Graph", &open, flags))
            {
                const StatHistory& cpuHist = NestedTimingTree::GetTotalCpuHistory();
                const StatHistory& gpuHist = NestedTimingTree::GetTotalGpuHistory();
                float maxRange = std::max(cpuHist.GetMax(), gpuHist.GetMax()) * 1.1f;
                if (maxRange <= 0.0f)
                    maxRange = 1.0f;

                ImGui::PlotLines("CPU ms", cpuHist.GetHistory(), cpuHist.GetHistoryLength(), 0, nullptr, 0.0f, maxRange, ImVec2(0, 80));
                ImGui::PlotLines("GPU ms", gpuHist.GetHistory(), gpuHist.GetHistoryLength(), 0, nullptr, 0.0f, maxRange, ImVec2(0, 80));
            }
            ImGui::End();

            rightY += graphHeight + margin;

            if (open != (bool)DrawPerfGraph)
            {
                if (open)
                    DrawPerfGraph.Increment();
                else
                    DrawPerfGraph.Decrement();
            }
        }

        if (DrawProfiler)
        {
            bool open = DrawProfiler;
            const float profilerHeight = std::max(240.0f, io.DisplaySize.y - rightY - margin);
            ImGui::SetNextWindowPos(ImVec2(rightX, rightY), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(rightWidth, profilerHeight), ImGuiCond_FirstUseEver);
            ImGuiWindowFlags flags = 0;
            if (ImGui::Begin("Profiler", &open, flags))
            {
                ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
                if (ImGui::BeginTable("ProfilerTable", 3, tableFlags))
                {
                    ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableHeadersRow();

                    NestedTimingTree::RenderImGuiTree();

                    ImGui::EndTable();
                }
            }
            ImGui::End();

            if (open != (bool)DrawProfiler)
            {
                if (open)
                    DrawProfiler.Increment();
                else
                    DrawProfiler.Decrement();
            }
        }
    }

} // EngineProfiling

void NestedTimingTree::PushProfilingMarker( const wstring& name, CommandContext* Context )
{
    sm_CurrentNode = sm_CurrentNode->GetChild(name);
    sm_CurrentNode->StartTiming(Context);
}

void NestedTimingTree::PopProfilingMarker( CommandContext* Context )
{
    sm_CurrentNode->StopTiming(Context);
    sm_CurrentNode = sm_CurrentNode->m_Parent;
}

void NestedTimingTree::RenderImGuiRow( int depth )
{
    if (this == &sm_RootScope)
    {
        for (auto node : m_Children)
            node->RenderImGuiRow(depth);
        return;
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (m_Children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (depth <= 1)
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    std::string label = Utility::WideStringToUTF8(m_Name);
    bool open = ImGui::TreeNodeEx(this, flags, "%s", label.c_str());

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    {
        ImGui::BeginTooltip();
        ImGui::Text("Avg CPU %.3f ms | Avg GPU %.3f ms", m_CpuTime.GetAvg(), m_GpuTime.GetAvg());
        float maxRange = std::max(m_CpuTime.GetMax(), m_GpuTime.GetMax()) * 1.1f;
        if (maxRange <= 0.0f)
            maxRange = 1.0f;
        ImGui::PlotLines("CPU", m_CpuTime.GetHistory(), m_CpuTime.GetHistoryLength(), 0, nullptr, 0.0f, maxRange, ImVec2(220, 60));
        ImGui::PlotLines("GPU", m_GpuTime.GetHistory(), m_GpuTime.GetHistoryLength(), 0, nullptr, 0.0f, maxRange, ImVec2(220, 60));
        ImGui::EndTooltip();
    }

    ImGui::TableNextColumn();
    ImGui::Text("%6.3f", m_CpuTime.GetAvg());
    ImGui::TableNextColumn();
    ImGui::Text("%6.3f", m_GpuTime.GetAvg());

    if (!m_Children.empty() && open)
    {
        for (auto node : m_Children)
            node->RenderImGuiRow(depth + 1);
        ImGui::TreePop();
    }
}

