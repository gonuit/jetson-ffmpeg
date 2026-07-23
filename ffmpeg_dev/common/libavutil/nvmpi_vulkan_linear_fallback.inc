        /* nvmpi: linear-layout fallback. Tegra answers the modifier query
         * without plane info for LINEAR images; use the subresource layout. */
        if (!drm_desc->layers[i].nb_planes &&
            f->tiling == VK_IMAGE_TILING_LINEAR) {
            int fb_planes = (nb_images == 1) ?
                            av_pix_fmt_count_planes(hwfc->sw_format) : 1;
            drm_desc->layers[i].nb_planes = fb_planes;
            for (int j = 0; j < fb_planes; j++) {
                VkSubresourceLayout layout;
                VkImageSubresource sub = {
                    .aspectMask = fb_planes == 1 ? VK_IMAGE_ASPECT_COLOR_BIT :
                                  j == 0 ? VK_IMAGE_ASPECT_PLANE_0_BIT :
                                  j == 1 ? VK_IMAGE_ASPECT_PLANE_1_BIT :
                                           VK_IMAGE_ASPECT_PLANE_2_BIT,
                };
                drm_desc->layers[i].planes[j].object_index = FFMIN(i, drm_desc->nb_objects - 1);
                vk->GetImageSubresourceLayout(hwctx->act_dev, f->img[i], &sub, &layout);
                drm_desc->layers[i].planes[j].offset = layout.offset;
                drm_desc->layers[i].planes[j].pitch  = layout.rowPitch;
            }
            drm_desc->objects[FFMIN(i, drm_desc->nb_objects - 1)].format_modifier = DRM_FORMAT_MOD_LINEAR;
        }

