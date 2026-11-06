#pragma once

#include "Display.h"
#include "TemporalEffects.h"
#include <memory>
#include <string>

class ColorBuffer;
class DepthBuffer;

namespace DlssBackend
{
    struct Status
    {
        bool available = false;
        unsigned int minimumDriverMajor = 0;
        unsigned int minimumDriverMinor = 0;
        std::string reason;
        uint64_t featureGeneration = 0;
        uint64_t evaluationCount = 0;
    };

    // One device/viewport. NGX types stay inside Impl. Fixed inputs: HDR RGBA16F,
    // reverse-Z depth, unjittered render-pixel RG16F motion, R32F exposure.
    class Context
    {
    public:
        Context();
        ~Context();
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        bool Initialize();
        PixelSize QueryRenderSize(TemporalEffects::Mode mode, PixelSize output);
        bool Configure(TemporalEffects::Mode mode, const FrameResolution& resolution);
        // Submit scene producers first. This owns a separate Direct command list.
        bool Evaluate(ColorBuffer& color, DepthBuffer& depth, ColorBuffer& motion,
            ColorBuffer& exposure, ColorBuffer& output,
            const TemporalEffects::TemporalFrameState& frame, float deltaSeconds);
        void ReleaseFeature();
        void Shutdown();
        const Status& GetStatus() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
