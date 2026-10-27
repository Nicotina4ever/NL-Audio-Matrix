#pragma once

// Engine v2 buffering policy.
//
// The router graph is produced by the master ASIO callback and consumed by
// several independent device paths (NLMatrix driver capture rings, WASAPI
// render endpoints, additional ASIO devices).  No two devices share a clock,
// and a user-mode thread is not a hard real-time entity, so every consumer
// path runs a level servo on top of a ring buffer:
//
//   producer --ring(target, adaptive)--> converter --> device
//
// - ringTarget*: adaptive fill target between the graph producer and the
//   device consumer.  It starts at ringTargetMinMs and grows (up to
//   ringTargetMaxMs) when the observed consumer starvation or the measured
//   ASIO callback jitter demands more headroom.  It is the "latency brake":
//   a well-behaved machine stays low, a jittery one trades latency for
//   continuity instead of producing dropouts.
// - driverTarget*: target fill of the kernel capture ring inside the
//   NLMatrix virtual driver (router -> NL Mic).  The writer thread keeps
//   this level with a servo, absorbing scheduling jitter of the writer
//   itself without ever letting the kernel ring underrun.
// - extraAsioTargetMs / renderTargetMs: same idea for secondary devices.
// - writeChunkMs: maximum amount of audio written to a device per servo
//   iteration (catch-up is allowed in one step so a late writer can restore
//   the target immediately).
// - servoIntervalMs: servo period; 2 ms with a high-resolution timer.
//
// Buffer profiles exposed to the GUI:
//   low       - smallest buffers, best for a native single-device setup;
//   balanced  - default, absorbs normal Windows scheduling jitter;
//   safe      - large headroom for VM / USB passthrough / heavy load;
//   advanced  - explicit numeric fields, no preset.
//
// All values are milliseconds; 0 in the JSON means "keep the preset value".

#include <juce_core/juce_core.h>

namespace nlmatrix
{
    struct EngineTuning
    {
        juce::String mode = "balanced";

        // Default preset is "balanced": ~110 ms of engine buffering, enough
        // for ordinary Windows scheduling jitter; low/safe trade latency for
        // headroom.  The target is primed once at engine start: increasing it
        // later would require inserting silence (a gap), so misconfigured
        // modes are reported instead of silently growing.
        int ringTargetMinMs = 60;
        int ringTargetMaxMs = 200;
        int driverTargetMs = 50;
        int driverTargetMaxMs = 250;
        int extraAsioTargetMs = 50;
        int renderTargetMs = 50;
        int writeChunkMs = 5;
        int servoIntervalMs = 2;

        static EngineTuning preset (const juce::String& name)
        {
            EngineTuning t;
            t.mode = name;
            if (name == "low")
            {
                // Native single-device setup: ~45 ms of engine buffering.
                t.ringTargetMinMs = 20;
                t.ringTargetMaxMs = 60;
                t.driverTargetMs = 25;
                t.driverTargetMaxMs = 80;
                t.extraAsioTargetMs = 20;
                t.renderTargetMs = 20;
                t.writeChunkMs = 5;
                t.servoIntervalMs = 2;
            }
            else if (name == "safe")
            {
                // VM / USB passthrough / loaded machine: ~250 ms of headroom.
                t.ringTargetMinMs = 150;
                t.ringTargetMaxMs = 400;
                t.driverTargetMs = 100;
                t.driverTargetMaxMs = 400;
                t.extraAsioTargetMs = 100;
                t.renderTargetMs = 100;
                t.writeChunkMs = 10;
                t.servoIntervalMs = 2;
            }
            return t;
        }

        static EngineTuning fromVar (const juce::var& section)
        {
            EngineTuning t = preset ("balanced");
            auto* o = section.getDynamicObject();
            if (o == nullptr)
                return t;

            auto get = [o] (const char* key) { return o->getProperty (juce::Identifier (key)); };

            const auto modeVar = get ("bufferMode");
            if (! modeVar.isVoid())
            {
                const auto m = modeVar.toString().trim().toLowerCase();
                if (m == "low" || m == "lowlatency" || m == "low latency")
                    t = preset ("low");
                else if (m == "safe")
                    t = preset ("safe");
                else if (m == "advanced")
                {
                    t = preset ("balanced");
                    t.mode = "advanced";
                }
                else
                    t = preset ("balanced");
            }

            auto overrideInt = [&] (const char* key, int& destination, int lo, int hi)
            {
                const auto v = get (key);
                if (v.isVoid())
                    return;
                const int value = (int) v.toString().getIntValue();
                if (value > 0)
                    destination = juce::jlimit (lo, hi, value);
            };

            overrideInt ("ringTargetMinMs", t.ringTargetMinMs, 5, 500);
            overrideInt ("ringTargetMaxMs", t.ringTargetMaxMs, t.ringTargetMinMs, 1000);
            overrideInt ("driverTargetMs", t.driverTargetMs, 10, 500);
            overrideInt ("driverTargetMaxMs", t.driverTargetMaxMs, t.driverTargetMs, 1000);
            overrideInt ("extraAsioTargetMs", t.extraAsioTargetMs, 5, 500);
            overrideInt ("renderTargetMs", t.renderTargetMs, 5, 500);
            overrideInt ("writeChunkMs", t.writeChunkMs, 1, 50);
            overrideInt ("servoIntervalMs", t.servoIntervalMs, 1, 20);

            return t;
        }

        juce::var toVar() const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("mode", mode);
            o->setProperty ("ringTargetMinMs", ringTargetMinMs);
            o->setProperty ("ringTargetMaxMs", ringTargetMaxMs);
            o->setProperty ("driverTargetMs", driverTargetMs);
            o->setProperty ("driverTargetMaxMs", driverTargetMaxMs);
            o->setProperty ("extraAsioTargetMs", extraAsioTargetMs);
            o->setProperty ("renderTargetMs", renderTargetMs);
            o->setProperty ("writeChunkMs", writeChunkMs);
            o->setProperty ("servoIntervalMs", servoIntervalMs);
            return juce::var (o);
        }

        bool operator== (const EngineTuning& other) const
        {
            return mode == other.mode
                   && ringTargetMinMs == other.ringTargetMinMs
                   && ringTargetMaxMs == other.ringTargetMaxMs
                   && driverTargetMs == other.driverTargetMs
                   && driverTargetMaxMs == other.driverTargetMaxMs
                   && extraAsioTargetMs == other.extraAsioTargetMs
                   && renderTargetMs == other.renderTargetMs
                   && writeChunkMs == other.writeChunkMs
                   && servoIntervalMs == other.servoIntervalMs;
        }
        bool operator!= (const EngineTuning& other) const { return ! (*this == other); }
    };
}
