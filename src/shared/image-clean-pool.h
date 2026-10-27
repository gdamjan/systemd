/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "forward.h"
#include "runtime-scope.h"

typedef enum ImageCleanPoolMode {
        IMAGE_CLEAN_POOL_REMOVE_ALL,
        IMAGE_CLEAN_POOL_REMOVE_HIDDEN,
        _IMAGE_CLEAN_POOL_MAX,
        _IMAGE_CLEAN_POOL_INVALID = -EINVAL,
} ImageCleanPoolMode;

DECLARE_STRING_TABLE_LOOKUP(image_clean_pool_mode, ImageCleanPoolMode);

int image_collect_mstack_layers(Hashmap *images, Set **ret);
bool image_is_mstack_layer(const Image *image, Set *mstack_layers);

int image_clean_pool_remove(Image **images, size_t n_images, RuntimeScope scope, int result_fd);

int clean_pool_read_first_entry(FILE *file, int child_error, sd_bus_error *error);
int clean_pool_read_next_entry(FILE *file, char **ret_name, uint64_t *ret_usage);
int image_clean_pool_reply(sd_bus_message *message, int *result_fd, int child_error, sd_bus_error *error);
