#include <animations/animation_registry.h>

#include <animations/animation_is_active_binding.h>

#include <animations/null_animation.h>
#include <animations/text_animation.h>
#include <animations/zigzag_animation.h>
#include <animations/rainbow_animation.h>
#include <animations/my_eyes_animation.h>

namespace
{
    using TextAnimationIsActive = AnimationIsActiveBinding<Animation::Text, BtServiceId::Text>;

#if defined(CONFIG_ANIMATION_ZIGZAG)
    using ZigZagAnimationIsActive = AnimationIsActiveBinding<Animation::ZigZag, BtServiceId::ZigZag>;
#endif

#if defined(CONFIG_ANIMATION_RAINBOW)
    using RainbowAnimationIsActive = AnimationIsActiveBinding<Animation::Rainbow, BtServiceId::Rainbow>;
#endif

#if defined(CONFIG_ANIMATION_MY_EYES)
    using MyEyesAnimationIsActive = AnimationIsActiveBinding<Animation::MyEyes, BtServiceId::MyEyes>;
#endif

    BaseAnimation *null_animation_factory()
    {
        return NullAnimation::getInstance();
    }

    BaseAnimation *text_animation_factory()
    {
        return TextAnimation::getInstance();
    }

#if defined(CONFIG_ANIMATION_ZIGZAG)
    BaseAnimation *zigzag_animation_factory()
    {
        return ZigZagAnimation::getInstance();
    }
#endif

#if defined(CONFIG_ANIMATION_RAINBOW)
    BaseAnimation *rainbow_animation_factory()
    {
        return RainbowAnimation::getInstance();
    }
#endif

#if defined(CONFIG_ANIMATION_MY_EYES)
    BaseAnimation *my_eyes_animation_factory()
    {
        return MyEyesAnimation::getInstance();
    }
#endif
}

int animation_registry_register_defaults()
{
    animation_registry_reset();

    int ret = animation_registry_register(Animation::None, null_animation_factory);
    if (ret)
    {
        return ret;
    }

    ret = animation_registry_register(Animation::Text, text_animation_factory);
    if (ret)
    {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::Text, TextAnimationIsActive::setLocalActiveState);
    if (ret)
    {
        return ret;
    }

    text_animation_bind_default_dependencies();

#if defined(CONFIG_ANIMATION_ZIGZAG)
    ret = animation_registry_register(Animation::ZigZag, zigzag_animation_factory);
    if (ret)
    {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::ZigZag, ZigZagAnimationIsActive::setLocalActiveState);
    if (ret)
    {
        return ret;
    }

    zigzag_animation_bind_default_dependencies();
#endif

#if defined(CONFIG_ANIMATION_RAINBOW)
    ret = animation_registry_register(Animation::Rainbow, rainbow_animation_factory);
    if (ret)
    {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::Rainbow, RainbowAnimationIsActive::setLocalActiveState);
    if (ret)
    {
        return ret;
    }

    rainbow_animation_bind_default_dependencies();
#endif

#if defined(CONFIG_ANIMATION_MY_EYES)
    ret = animation_registry_register(Animation::MyEyes, my_eyes_animation_factory);
    if (ret)
    {
        return ret;
    }

    ret = animation_registry_register_is_active(Animation::MyEyes, MyEyesAnimationIsActive::setLocalActiveState);
    if (ret)
    {
        return ret;
    }
#endif

    return 0;
}
