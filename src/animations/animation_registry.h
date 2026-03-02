#pragma once

#include <animations/animation_base.h>
#include <animations/animation_types.h>

using AnimationInstanceFactory = BaseAnimation *(*)();

int animation_registry_register(Animation id, AnimationInstanceFactory factory);

void animation_registry_reset();

BaseAnimation *animation_registry_get(Animation id);

size_t animation_registry_count();

void animation_registry_init_registered();

int animation_registry_register_defaults();
