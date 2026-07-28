#pragma once

#include <animations/animation_base.h>
#include <animations/animation_types.h>

using AnimationInstanceFactory = BaseAnimation *(*)();
using AnimationIsActiveSetter = void (*)(bool active);
using AnimationShuffleIncludeGetter = bool (*)();

int animation_registry_register(Animation id, AnimationInstanceFactory factory);

int animation_registry_register_is_active(Animation id, AnimationIsActiveSetter setter);

/**
 * @brief Registers the per-animation "include in shuffle" getter (issue #243).
 *
 * Same contract as animation_registry_register_is_active(): returns -ENOENT unless
 * animation_registry_register() already created the id's entry, and the return value
 * must be checked — an ignored -ENOENT here silently exempts nothing and includes the
 * animation forever (cf. the PR #89 is-active incident).
 */
int animation_registry_register_shuffle_include(Animation id, AnimationShuffleIncludeGetter getter);

/**
 * @brief Whether shuffle may pick this animation (issue #243). Pulled by the shuffle
 * pool at pick time. Unknown ids and entries with no registered getter default to
 * true — an animation only leaves the shuffle pool by explicit opt-out.
 */
bool animation_registry_shuffle_included(Animation id);

void animation_registry_reset();

BaseAnimation *animation_registry_get(Animation id);

size_t animation_registry_count();

/**
 * @brief Registered animation id at `index` (registration order), or Animation::None if
 * index >= animation_registry_count(). Note index 0 is normally the real None entry
 * (registered first by animation_registry_register_defaults()) — callers filtering out
 * None, like the shuffle pool, are unaffected by the sentinel overloading.
 */
Animation animation_registry_id_at(size_t index);

void animation_registry_init_registered();

void animation_registry_set_is_active(Animation id, bool active);

int animation_registry_register_defaults();
