#pragma once

#include <animations/animation.h>

#include <cstddef>
#include <cstdint>

struct M3Environment;
struct M3Runtime;
struct M3Function;
struct M3ImportContext;

class WasmMvpAnimation : public BaseAnimationTemplate<WasmMvpAnimation, Animation::WasmMvp> {
   public:
    void init() override;
    void tick(AnimationRenderer& renderer, size_t timeSinceLastTickMs) override;

   private:
    static const void* fillHost(struct M3Runtime* runtime, struct M3ImportContext* context,
                                uint64_t* stack, void* memory);
    bool initializeRuntime();
    void fill(AnimationRenderer& renderer, uint32_t color);

    struct M3Environment* environment_ = nullptr;
    struct M3Runtime* runtime_ = nullptr;
    struct M3Function* tickFunction_ = nullptr;
    AnimationRenderer* activeRenderer_ = nullptr;
    uint32_t elapsedMs_ = 0;
    bool initializationAttempted_ = false;
    bool ready_ = false;
};
