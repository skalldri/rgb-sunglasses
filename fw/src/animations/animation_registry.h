#pragma once

#include <animations/animation_base.h>
#include <animations/animation_types.h>

using AnimationInstanceFactory = BaseAnimation *(*)();
using AnimationIsActiveSetter = void (*)(bool active);

int animation_registry_register(Animation id, AnimationInstanceFactory factory);

int animation_registry_register_is_active(Animation id, AnimationIsActiveSetter setter);

void animation_registry_reset();

BaseAnimation *animation_registry_get(Animation id);

size_t animation_registry_count();

void animation_registry_init_registered();

void animation_registry_set_is_active(Animation id, bool active);

int animation_registry_register_defaults();
