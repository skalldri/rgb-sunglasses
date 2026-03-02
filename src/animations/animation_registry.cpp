#include <animations/animation_registry.h>

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

namespace
{
    struct AnimationRegistryEntry
    {
        Animation id;
        AnimationInstanceFactory factory;
    };

    static constexpr size_t kMaxRegistryEntries = 16;
    static AnimationRegistryEntry sRegistry[kMaxRegistryEntries];
    static size_t sRegistryCount = 0;

    ssize_t findRegistryIndex(Animation id)
    {
        for (size_t i = 0; i < sRegistryCount; i++)
        {
            if (sRegistry[i].id == id)
            {
                return i;
            }
        }

        return -1;
    }
}

int animation_registry_register(Animation id, AnimationInstanceFactory factory)
{
    if (!factory)
    {
        return -EINVAL;
    }

    ssize_t idx = findRegistryIndex(id);
    if (idx >= 0)
    {
        sRegistry[idx].factory = factory;
        return 0;
    }

    if (sRegistryCount >= kMaxRegistryEntries)
    {
        return -ENOMEM;
    }

    sRegistry[sRegistryCount] = {
        .id = id,
        .factory = factory,
    };
    sRegistryCount++;
    return 0;
}

void animation_registry_reset()
{
    sRegistryCount = 0;
}

BaseAnimation *animation_registry_get(Animation id)
{
    ssize_t idx = findRegistryIndex(id);
    if (idx < 0)
    {
        return NULL;
    }

    return sRegistry[idx].factory();
}

size_t animation_registry_count()
{
    return sRegistryCount;
}

void animation_registry_init_registered()
{
    for (size_t i = 0; i < sRegistryCount; i++)
    {
        BaseAnimation *animation = sRegistry[i].factory();
        if (animation)
        {
            animation->init();
        }
    }
}
