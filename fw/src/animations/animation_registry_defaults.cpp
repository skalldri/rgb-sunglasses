#include <animations/animation_activator.h>
#include <animations/animation_active_state_observer.h>
#include <animations/animation_is_active_binding.h>
#include <animations/animation_registry.h>
#include <animations/my_eyes_animation.h>
#include <animations/null_animation.h>
#include <animations/rainbow_animation.h>
#include <animations/text_animation.h>
#include <animations/zigzag_animation.h>
#include <pattern_controller.h>

#if defined(CONFIG_ANIMATION_BEAT)
#include <animations/beat_animation.h>
#endif

#if defined(CONFIG_ANIMATION_FFT_BARS)
#include <animations/fft_bars_animation.h>
#endif

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
#include <animations/glim_player_animation.h>
#endif

namespace {
class RegistryActiveStateObserver : public AnimationActiveStateObserver {
   public:
    void onActiveStateChanged(Animation id, bool active) override {
        animation_registry_set_is_active(id, active);
    }
};

class PatternControllerActivator : public AnimationActivator {
   public:
    void changeToAnimation(Animation id) override { pattern_controller_change_to_animation(id); }

    void deactivateAnimation(Animation id) override {
        if (pattern_controller_get_current_animation() == id) {
            pattern_controller_change_to_animation(Animation::None);
        }
    }
};

static RegistryActiveStateObserver sRegistryObserver;
static PatternControllerActivator sActivator;

using TextAnimationIsActive = AnimationIsActiveBinding<Animation::Text>;

#if defined(CONFIG_ANIMATION_ZIGZAG)
using ZigZagAnimationIsActive = AnimationIsActiveBinding<Animation::ZigZag>;
#endif

#if defined(CONFIG_ANIMATION_RAINBOW)
using RainbowAnimationIsActive = AnimationIsActiveBinding<Animation::Rainbow>;
#endif

#if defined(CONFIG_ANIMATION_MY_EYES)
using MyEyesAnimationIsActive = AnimationIsActiveBinding<Animation::MyEyes>;
#endif

#if defined(CONFIG_ANIMATION_BEAT)
using BeatAnimationIsActive = AnimationIsActiveBinding<Animation::Beat>;
#endif

#if defined(CONFIG_ANIMATION_FFT_BARS)
using FftBarsAnimationIsActive = AnimationIsActiveBinding<Animation::FftBars>;
#endif

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
using GlimPlayerAnimationIsActive = AnimationIsActiveBinding<Animation::GlimPlayer>;
#endif

BaseAnimation *null_animation_factory() {
    return NullAnimation::getInstance();
}

BaseAnimation *text_animation_factory() {
    return TextAnimation::getInstance();
}

#if defined(CONFIG_ANIMATION_ZIGZAG)
BaseAnimation *zigzag_animation_factory() {
    return ZigZagAnimation::getInstance();
}
#endif

#if defined(CONFIG_ANIMATION_RAINBOW)
BaseAnimation *rainbow_animation_factory() {
    return RainbowAnimation::getInstance();
}
#endif

#if defined(CONFIG_ANIMATION_MY_EYES)
BaseAnimation *my_eyes_animation_factory() {
    return MyEyesAnimation::getInstance();
}
#endif

#if defined(CONFIG_ANIMATION_BEAT)
BaseAnimation *beat_animation_factory() {
    return BeatAnimation::getInstance();
}
#endif

#if defined(CONFIG_ANIMATION_FFT_BARS)
BaseAnimation *fft_bars_animation_factory() {
    return FftBarsAnimation::getInstance();
}
#endif

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
BaseAnimation *glim_player_animation_factory() {
    return GlimPlayerAnimation::getInstance();
}
#endif
}  // namespace

int animation_registry_register_defaults() {
    BaseAnimation::registerActiveStateObserver(&sRegistryObserver);
    AnimationIsActiveBinding<Animation::Text>::registerActivator(&sActivator);
#if defined(CONFIG_ANIMATION_ZIGZAG)
    AnimationIsActiveBinding<Animation::ZigZag>::registerActivator(&sActivator);
#endif
#if defined(CONFIG_ANIMATION_RAINBOW)
    AnimationIsActiveBinding<Animation::Rainbow>::registerActivator(&sActivator);
#endif
#if defined(CONFIG_ANIMATION_MY_EYES)
    AnimationIsActiveBinding<Animation::MyEyes>::registerActivator(&sActivator);
#endif
#if defined(CONFIG_ANIMATION_BEAT)
    AnimationIsActiveBinding<Animation::Beat>::registerActivator(&sActivator);
#endif
#if defined(CONFIG_ANIMATION_FFT_BARS)
    AnimationIsActiveBinding<Animation::FftBars>::registerActivator(&sActivator);
#endif
#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
    AnimationIsActiveBinding<Animation::GlimPlayer>::registerActivator(&sActivator);
#endif

    animation_registry_reset();

    int ret = animation_registry_register(Animation::None, null_animation_factory);
    if (ret) {
        return ret;
    }

    ret = animation_registry_register(Animation::Text, text_animation_factory);
    if (ret) {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::Text,
                                                TextAnimationIsActive::setLocalActiveState);
    if (ret) {
        return ret;
    }

    text_animation_bind_default_dependencies();

#if defined(CONFIG_ANIMATION_ZIGZAG)
    ret = animation_registry_register(Animation::ZigZag, zigzag_animation_factory);
    if (ret) {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::ZigZag,
                                                ZigZagAnimationIsActive::setLocalActiveState);
    if (ret) {
        return ret;
    }

    zigzag_animation_bind_default_dependencies();
#endif

#if defined(CONFIG_ANIMATION_RAINBOW)
    ret = animation_registry_register(Animation::Rainbow, rainbow_animation_factory);
    if (ret) {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::Rainbow,
                                                RainbowAnimationIsActive::setLocalActiveState);
    if (ret) {
        return ret;
    }

    rainbow_animation_bind_default_dependencies();
#endif

#if defined(CONFIG_ANIMATION_MY_EYES)
    ret = animation_registry_register(Animation::MyEyes, my_eyes_animation_factory);
    if (ret) {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::MyEyes,
                                                MyEyesAnimationIsActive::setLocalActiveState);
    if (ret) {
        return ret;
    }

    my_eyes_animation_bind_default_dependencies();
#endif

#if defined(CONFIG_ANIMATION_BEAT)
    ret = animation_registry_register(Animation::Beat, beat_animation_factory);
    if (ret) {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::Beat,
                                                BeatAnimationIsActive::setLocalActiveState);
    if (ret) {
        return ret;
    }

    beat_animation_bind_default_sound_dependencies();
    beat_animation_bind_default_bt_dependencies();
#endif

#if defined(CONFIG_ANIMATION_FFT_BARS)
    ret = animation_registry_register(Animation::FftBars, fft_bars_animation_factory);
    if (ret) {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::FftBars,
                                                FftBarsAnimationIsActive::setLocalActiveState);
    if (ret) {
        return ret;
    }

    fft_bars_animation_bind_default_sound_dependencies();
    fft_bars_animation_bind_default_bt_dependencies();
#endif

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
    ret = animation_registry_register(Animation::GlimPlayer, glim_player_animation_factory);
    if (ret) {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::GlimPlayer,
                                                GlimPlayerAnimationIsActive::setLocalActiveState);
    if (ret) {
        return ret;
    }

    glim_player_animation_bind_default_button_dependencies();
    glim_player_animation_bind_default_bt_dependencies();
#endif

    return 0;
}
