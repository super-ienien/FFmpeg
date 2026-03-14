/**
 * @file videomaster_enc.h
 * @brief This file contains the function declaration for VideoMaster DELTACAST
 * (c) output devices.
 * @version 1.0
 * @date 2025-05-13
 *
 * @copyright Copyright (c) 2025
 *
 * This file is part of FFmpeg and use VideoMaster DELTACAST (c) API to
 * communicate with PCIe DELTACAST(c) devices.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVDEVICE_VIDEOMASTER_ENC_H
#define AVDEVICE_VIDEOMASTER_ENC_H

#include <stdbool.h>

#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"
#if defined(__APPLE__)
#include <VideoMasterHD/VideoMasterHD_Core.h>
#include <VideoMasterHD/VideoMasterHD_Sdi.h>
#else
#include <VideoMasterHD_Core.h>
#include <VideoMasterHD_Sdi.h>
#endif

/**
 * @brief Lists available VideoMaster DELTACAST(c) output devices.
 *
 * Populates the device list with available TX devices.
 *
 * @param avctx FFmpeg context for the device.
 * @param device_list List to be populated.
 * @return 0 on success, negative AVERROR on failure.
 */
int ff_videomaster_list_output_devices(AVFormatContext         *avctx,
                                       struct AVDeviceInfoList *device_list);

/**
 * @brief Initializes the VideoMaster DELTACAST(c) TX device.
 *
 * Opens board, configures stream, keyer, and starts preroll.
 *
 * @param avctx FFmpeg context for the device.
 * @return 0 on success, negative AVERROR on failure.
 */
int ff_videomaster_write_header(AVFormatContext *avctx);

/**
 * @brief Writes a video or audio packet to the TX device.
 *
 * @param avctx FFmpeg context for the device.
 * @param pkt Packet to write.
 * @return 0 on success, negative AVERROR on failure.
 */
int ff_videomaster_write_packet(AVFormatContext *avctx, AVPacket *pkt);

/**
 * @brief Stops the TX stream and releases resources.
 *
 * @param avctx FFmpeg context for the device.
 * @return 0 on success, negative AVERROR on failure.
 */
int ff_videomaster_write_trailer(AVFormatContext *avctx);

#endif /* AVDEVICE_VIDEOMASTER_ENC_H */
