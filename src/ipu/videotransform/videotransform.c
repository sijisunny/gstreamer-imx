/* Video transform element using the Freescale IPU
 * Copyright (C) 2013  Carlos Rafael Giani
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideopool.h>
#include <gst/video/gstvideometa.h>

#include "videotransform.h"
#include "../common/phys_mem_meta.h"
#include "../blitter.h"
#include "../allocator.h"




GST_DEBUG_CATEGORY_STATIC(imx_ipu_video_transform_debug);
#define GST_CAT_DEFAULT imx_ipu_video_transform_debug


enum
{
	PROP_0,
	PROP_OUTPUT_ROTATION,
	PROP_INPUT_CROP,
	PROP_DEINTERLACE_MODE
};


static GstStaticPadTemplate static_sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_IMX_IPU_BLITTER_CAPS
);


static GstStaticPadTemplate static_src_template = GST_STATIC_PAD_TEMPLATE(
	"src",
	GST_PAD_SRC,
	GST_PAD_ALWAYS,
	GST_IMX_IPU_BLITTER_CAPS
);


struct _GstImxIpuVideoTransformPrivate
{
	GstImxIpuBlitter *blitter;
	GMutex mutex;

	GstImxIpuBlitterRotationMode output_rotation;
	gboolean input_crop;
	GstImxIpuBlitterDeinterlaceMode deinterlace_mode;
};


#define LOCK_BLITTER_MUTEX(obj) g_mutex_lock(&(((GstImxIpuVideoTransform*)obj)->priv->mutex))
#define UNLOCK_BLITTER_MUTEX(obj) g_mutex_unlock(&(((GstImxIpuVideoTransform*)obj)->priv->mutex))


G_DEFINE_TYPE(GstImxIpuVideoTransform, gst_imx_ipu_video_transform, GST_TYPE_VIDEO_FILTER)


static GstStateChangeReturn gst_imx_ipu_video_transform_change_state(GstElement *element, GstStateChange transition);
static void gst_imx_ipu_video_transform_init_device(GstImxIpuVideoTransform *ipu_video_transform);
static void gst_imx_ipu_video_transform_uninit_device(GstImxIpuVideoTransform *ipu_video_transform);
static void gst_imx_ipu_video_transform_finalize(GObject *object);
static void gst_imx_ipu_video_transform_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec);
static void gst_imx_ipu_video_transform_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static gboolean gst_imx_ipu_video_transform_src_event(GstBaseTransform *transform, GstEvent *event);
static GstCaps* gst_imx_ipu_video_transform_transform_caps(GstBaseTransform *transform, GstPadDirection direction, GstCaps *caps, GstCaps *filter);
static GstCaps* gst_imx_ipu_video_transform_fixate_caps(GstBaseTransform *transform, GstPadDirection direction, GstCaps *caps, GstCaps *othercaps);
static GstCaps* gst_imx_ipu_video_transform_fixate_size_caps(GstBaseTransform *transform, GstPadDirection direction, GstCaps *caps, GstCaps *othercaps);
static void gst_imx_ipu_video_transform_fixate_format_caps(GstBaseTransform *transform, GstCaps *caps, GstCaps *othercaps);
static gboolean gst_imx_ipu_video_transform_propose_allocation(GstBaseTransform *transform, GstQuery *decide_query, GstQuery *query);
static gboolean gst_imx_ipu_video_transform_decide_allocation(GstBaseTransform *transform, GstQuery *query);
static gboolean gst_imx_ipu_video_transform_set_info(GstVideoFilter *filter, GstCaps *in, GstVideoInfo *in_info, GstCaps *out, GstVideoInfo *out_info);
static GstFlowReturn gst_imx_ipu_video_transform_prepare_output_buffer(GstBaseTransform * trans, GstBuffer *input, GstBuffer **outbuf);
static GstFlowReturn gst_imx_ipu_video_transform_transform_frame(GstVideoFilter *filter, GstVideoFrame *in, GstVideoFrame *out);




/* required function declared by G_DEFINE_TYPE */

void gst_imx_ipu_video_transform_class_init(GstImxIpuVideoTransformClass *klass)
{
	GObjectClass *object_class;
	GstBaseTransformClass *base_transform_class;
	GstVideoFilterClass *video_filter_class;
	GstElementClass *element_class;

	GST_DEBUG_CATEGORY_INIT(imx_ipu_video_transform_debug, "imxipuvideotransform", 0, "Freescale i.MX IPU video transform");

	object_class = G_OBJECT_CLASS(klass);
	base_transform_class = GST_BASE_TRANSFORM_CLASS(klass);
	video_filter_class = GST_VIDEO_FILTER_CLASS(klass);
	element_class = GST_ELEMENT_CLASS(klass);

	gst_element_class_set_static_metadata(
		element_class,
		"Freescale IPU video transform element",
		"Filter/Converter/Video/Scaler",
		"Video frame transfomrations using the Freescale IPU",
		"Carlos Rafael Giani <dv@pseudoterminal.org>"
	);

	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&static_sink_template));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&static_src_template));

	element_class->change_state                 = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_change_state);
	object_class->finalize                      = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_finalize);
	object_class->set_property                  = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_set_property);
	object_class->get_property                  = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_get_property);
	base_transform_class->src_event             = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_src_event);
	base_transform_class->transform_caps        = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_transform_caps);
	base_transform_class->fixate_caps           = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_fixate_caps);
	base_transform_class->propose_allocation    = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_propose_allocation);
	base_transform_class->decide_allocation     = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_decide_allocation);
	base_transform_class->prepare_output_buffer = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_prepare_output_buffer);
	video_filter_class->set_info                = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_set_info);
	video_filter_class->transform_frame         = GST_DEBUG_FUNCPTR(gst_imx_ipu_video_transform_transform_frame);

	base_transform_class->passthrough_on_same_caps = FALSE;

	g_object_class_install_property(
		object_class,
		PROP_OUTPUT_ROTATION,
		g_param_spec_enum(
			"output-rotation",
			"Output rotation",
			"Rotation that shall be applied to output frames",
			gst_imx_ipu_blitter_rotation_mode_get_type(),
			GST_IMX_IPU_BLITTER_OUTPUT_ROTATION_DEFAULT,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
	g_object_class_install_property(
		object_class,
		PROP_INPUT_CROP,
		g_param_spec_boolean(
			"enable-crop",
			"Enable input frame cropping",
			"Whether or not to crop input frames based on their video crop metadata",
			GST_IMX_IPU_BLITTER_CROP_DEFAULT,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
	g_object_class_install_property(
		object_class,
		PROP_DEINTERLACE_MODE,
		g_param_spec_enum(
			"deinterlace-mode",
			"Deinterlace mode",
			"Deinterlacing mode to be used for incoming frames (ignored if frames are not interlaced)",
			gst_imx_ipu_blitter_deinterlace_mode_get_type(),
			GST_IMX_IPU_BLITTER_DEINTERLACE_DEFAULT,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
		)
	);
}


void gst_imx_ipu_video_transform_init(GstImxIpuVideoTransform *ipu_video_transform)
{
	GstBaseTransform *base_transform = GST_BASE_TRANSFORM(ipu_video_transform);

	ipu_video_transform->priv = g_slice_alloc(sizeof(GstImxIpuVideoTransformPrivate));
	ipu_video_transform->priv->blitter = NULL;
	g_mutex_init(&(ipu_video_transform->priv->mutex));
	ipu_video_transform->inout_caps_equal = FALSE;

	ipu_video_transform->priv->output_rotation = GST_IMX_IPU_BLITTER_OUTPUT_ROTATION_DEFAULT;
	ipu_video_transform->priv->input_crop = GST_IMX_IPU_BLITTER_CROP_DEFAULT;
	ipu_video_transform->priv->deinterlace_mode = GST_IMX_IPU_BLITTER_DEINTERLACE_DEFAULT;

	/* Set passthrough initially to FALSE ; passthrough will later be
	 * enabled/disabled on a per-frame basis */
	gst_base_transform_set_passthrough(base_transform, FALSE);
}


static GstStateChangeReturn gst_imx_ipu_video_transform_change_state(GstElement *element, GstStateChange transition)
{
	GstImxIpuVideoTransform *ipu_video_transform = GST_IMX_IPU_VIDEO_TRANSFORM(element);
	GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

	switch (transition)
	{
		case GST_STATE_CHANGE_NULL_TO_READY:
		{
			LOCK_BLITTER_MUTEX(ipu_video_transform);
			gst_imx_ipu_video_transform_init_device(ipu_video_transform);
			UNLOCK_BLITTER_MUTEX(ipu_video_transform);
			break;
		}

		default:
			break;
	}

	ret = GST_ELEMENT_CLASS(gst_imx_ipu_video_transform_parent_class)->change_state(element, transition);
	if (ret == GST_STATE_CHANGE_FAILURE)
		return ret;

	switch (transition)
	{
		case GST_STATE_CHANGE_READY_TO_NULL:
		{
			LOCK_BLITTER_MUTEX(ipu_video_transform);
			gst_imx_ipu_video_transform_uninit_device(ipu_video_transform);
			UNLOCK_BLITTER_MUTEX(ipu_video_transform);
			break;
		}

		default:
			break;
	}

	return ret;
}


static void gst_imx_ipu_video_transform_init_device(GstImxIpuVideoTransform *ipu_video_transform)
{
	/* must be called with lock */

	g_assert(ipu_video_transform->priv != NULL);
	ipu_video_transform->priv->blitter = g_object_new(gst_imx_ipu_blitter_get_type(), NULL);

	gst_imx_ipu_blitter_set_output_rotation_mode(ipu_video_transform->priv->blitter, ipu_video_transform->priv->output_rotation);
	gst_imx_ipu_blitter_enable_crop(ipu_video_transform->priv->blitter, ipu_video_transform->priv->input_crop);
	gst_imx_ipu_blitter_set_deinterlace_mode(ipu_video_transform->priv->blitter, ipu_video_transform->priv->deinterlace_mode);
}


static void gst_imx_ipu_video_transform_uninit_device(GstImxIpuVideoTransform *ipu_video_transform)
{
	/* must be called with lock */

	if (ipu_video_transform->priv != NULL)
	{
		if (ipu_video_transform->priv->blitter != NULL)
			gst_object_unref(ipu_video_transform->priv->blitter);
		ipu_video_transform->priv->blitter = NULL;
	}
}


static void gst_imx_ipu_video_transform_finalize(GObject *object)
{
	GstImxIpuVideoTransform *ipu_video_transform = GST_IMX_IPU_VIDEO_TRANSFORM(object);

	gst_imx_ipu_video_transform_uninit_device(ipu_video_transform);
	if (ipu_video_transform->priv != NULL)
	{
		g_mutex_clear(&(ipu_video_transform->priv->mutex));
		g_slice_free1(sizeof(GstImxIpuVideoTransformPrivate), ipu_video_transform->priv);
	}

	G_OBJECT_CLASS(gst_imx_ipu_video_transform_parent_class)->finalize(object);
}


static void gst_imx_ipu_video_transform_set_property(GObject *object, guint prop_id, GValue const *value, GParamSpec *pspec)
{
	GstImxIpuVideoTransform *ipu_video_transform = GST_IMX_IPU_VIDEO_TRANSFORM(object);

	switch (prop_id)
	{
		case PROP_OUTPUT_ROTATION:
			LOCK_BLITTER_MUTEX(ipu_video_transform);
			ipu_video_transform->priv->output_rotation = g_value_get_enum(value);
			if (ipu_video_transform->priv->blitter != NULL)
				gst_imx_ipu_blitter_set_output_rotation_mode(ipu_video_transform->priv->blitter, ipu_video_transform->priv->output_rotation);
			UNLOCK_BLITTER_MUTEX(ipu_video_transform);
			break;

		case PROP_INPUT_CROP:
			LOCK_BLITTER_MUTEX(ipu_video_transform);
			ipu_video_transform->priv->input_crop = g_value_get_boolean(value);
			if (ipu_video_transform->priv->blitter != NULL)
				gst_imx_ipu_blitter_enable_crop(ipu_video_transform->priv->blitter, ipu_video_transform->priv->input_crop);
			UNLOCK_BLITTER_MUTEX(ipu_video_transform);
			break;

		case PROP_DEINTERLACE_MODE:
			LOCK_BLITTER_MUTEX(ipu_video_transform);
			ipu_video_transform->priv->deinterlace_mode = g_value_get_enum(value);
			if (ipu_video_transform->priv->blitter != NULL)
				gst_imx_ipu_blitter_set_deinterlace_mode(ipu_video_transform->priv->blitter, ipu_video_transform->priv->deinterlace_mode);
			UNLOCK_BLITTER_MUTEX(ipu_video_transform);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static void gst_imx_ipu_video_transform_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GstImxIpuVideoTransform *ipu_video_transform = GST_IMX_IPU_VIDEO_TRANSFORM(object);

	switch (prop_id)
	{
		case PROP_OUTPUT_ROTATION:
			LOCK_BLITTER_MUTEX(ipu_video_transform);
			g_value_set_enum(value, ipu_video_transform->priv->output_rotation);
			UNLOCK_BLITTER_MUTEX(ipu_video_transform);
			break;

		case PROP_INPUT_CROP:
			LOCK_BLITTER_MUTEX(ipu_video_transform);
			g_value_set_boolean(value, ipu_video_transform->priv->input_crop);
			UNLOCK_BLITTER_MUTEX(ipu_video_transform);
			break;

		case PROP_DEINTERLACE_MODE:
			LOCK_BLITTER_MUTEX(ipu_video_transform);
			g_value_set_enum(value, ipu_video_transform->priv->deinterlace_mode);
			UNLOCK_BLITTER_MUTEX(ipu_video_transform);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
			break;
	}
}


static gboolean gst_imx_ipu_video_transform_src_event(GstBaseTransform *transform, GstEvent *event)
{
	gdouble a;
	GstStructure *structure;
	GstVideoFilter *filter = GST_VIDEO_FILTER_CAST(transform);

	GST_DEBUG_OBJECT(transform, "handling %s event", GST_EVENT_TYPE_NAME(event));

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_NAVIGATION:
			if ((filter->in_info.width != filter->out_info.width) || (filter->in_info.height != filter->out_info.height))
			{
				event = GST_EVENT(gst_mini_object_make_writable(GST_MINI_OBJECT(event)));

				structure = (GstStructure *)gst_event_get_structure(event);
				if (gst_structure_get_double(structure, "pointer_x", &a))
				{
					gst_structure_set(
						structure,
						"pointer_x",
						G_TYPE_DOUBLE,
						a * filter->in_info.width / filter->out_info.width,
						NULL
					);
				}
				if (gst_structure_get_double(structure, "pointer_y", &a))
				{
					gst_structure_set(
						structure,
						"pointer_y",
						G_TYPE_DOUBLE,
						a * filter->in_info.height / filter->out_info.height,
						NULL
					);
				}
			}
			break;
		default:
			break;
	}

	return GST_BASE_TRANSFORM_CLASS(gst_imx_ipu_video_transform_parent_class)->src_event(transform, event);
}


static GstCaps* gst_imx_ipu_video_transform_transform_caps(GstBaseTransform *transform, G_GNUC_UNUSED GstPadDirection direction, GstCaps *caps, GstCaps *filter)
{
	GstCaps *tmpcaps1, *tmpcaps2, *result;
	GstStructure *structure;
	gint i, n;

	tmpcaps1 = gst_caps_new_empty();
	n = gst_caps_get_size(caps);
	for (i = 0; i < n; i++)
	{
		structure = gst_caps_get_structure(caps, i);

		/* If this is already expressed by the existing caps
		 * skip this structure */
		if ((i > 0) && gst_caps_is_subset_structure(tmpcaps1, structure))
			continue;

		/* make copy */
		structure = gst_structure_copy(structure);
		gst_structure_set(
			structure,
			"width", GST_TYPE_INT_RANGE, 64, G_MAXINT,
			"height", GST_TYPE_INT_RANGE, 64, G_MAXINT,
			NULL
		);

		gst_structure_remove_fields(structure, "format", "colorimetry", "chroma-site", NULL);

		/* if pixel aspect ratio, make a range of it */
		if (gst_structure_has_field(structure, "pixel-aspect-ratio"))
		{
			gst_structure_set(
				structure,
				"pixel-aspect-ratio", GST_TYPE_FRACTION_RANGE, 1, G_MAXINT, G_MAXINT, 1,
				NULL
			);
		}
		gst_caps_append_structure(tmpcaps1, structure);
	}

	if (filter != NULL)
	{
		tmpcaps2 = gst_caps_intersect_full(filter, tmpcaps1, GST_CAPS_INTERSECT_FIRST);
		gst_caps_unref(tmpcaps1);
		tmpcaps1 = tmpcaps2;
	}

	result = tmpcaps1;

	GST_DEBUG_OBJECT(transform, "transformed %" GST_PTR_FORMAT " into %" GST_PTR_FORMAT, (gpointer)caps, (gpointer)result);

	return result;
}


static GstCaps* gst_imx_ipu_video_transform_fixate_caps(GstBaseTransform *transform, GstPadDirection direction, GstCaps *caps, GstCaps *othercaps)
{
	othercaps = gst_caps_truncate(othercaps);
	othercaps = gst_caps_make_writable(othercaps);

	GST_DEBUG_OBJECT(transform, "trying to fixate othercaps %" GST_PTR_FORMAT " based on caps %" GST_PTR_FORMAT, (gpointer)othercaps, (gpointer)caps);

	othercaps = gst_imx_ipu_video_transform_fixate_size_caps(transform, direction, caps, othercaps);
	gst_imx_ipu_video_transform_fixate_format_caps(transform, caps, othercaps);

	return othercaps;
}


static GstCaps* gst_imx_ipu_video_transform_fixate_size_caps(GstBaseTransform *transform, GstPadDirection direction, GstCaps *caps, GstCaps *othercaps)
{
	GstStructure *ins, *outs;
	GValue const *from_par, *to_par;
	GValue fpar = { 0, }, tpar = { 0, };

	ins = gst_caps_get_structure(caps, 0);
	outs = gst_caps_get_structure(othercaps, 0);

	from_par = gst_structure_get_value(ins, "pixel-aspect-ratio");
	to_par = gst_structure_get_value(outs, "pixel-aspect-ratio");

	/* If we're fixating from the sinkpad we always set the PAR and
	 * assume that missing PAR on the sinkpad means 1/1 and
	 * missing PAR on the srcpad means undefined
	 */
	if (direction == GST_PAD_SINK)
	{
		if (!from_par)
		{
			g_value_init(&fpar, GST_TYPE_FRACTION);
			gst_value_set_fraction(&fpar, 1, 1);
			from_par = &fpar;
		}
		if (!to_par)
		{
			g_value_init(&tpar, GST_TYPE_FRACTION_RANGE);
			gst_value_set_fraction_range_full(&tpar, 1, G_MAXINT, G_MAXINT, 1);
			to_par = &tpar;
		}
	} else
	{
		if (!to_par)
		{
			g_value_init(&tpar, GST_TYPE_FRACTION);
			gst_value_set_fraction(&tpar, 1, 1);
			to_par = &tpar;

			gst_structure_set(outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
		}
		if (!from_par)
		{
			g_value_init(&fpar, GST_TYPE_FRACTION);
			gst_value_set_fraction (&fpar, 1, 1);
			from_par = &fpar;
		}
	}

	/* we have both PAR but they might not be fixated */
	{
		gint from_w, from_h, from_par_n, from_par_d, to_par_n, to_par_d;
		gint w = 0, h = 0;
		gint from_dar_n, from_dar_d;
		gint num, den;

		/* from_par should be fixed */
		g_return_val_if_fail(gst_value_is_fixed(from_par), othercaps);

		from_par_n = gst_value_get_fraction_numerator(from_par);
		from_par_d = gst_value_get_fraction_denominator(from_par);

		gst_structure_get_int(ins, "width", &from_w);
		gst_structure_get_int(ins, "height", &from_h);

		gst_structure_get_int(outs, "width", &w);
		gst_structure_get_int(outs, "height", &h);

		/* if both width and height are already fixed, we can't do anything
		 * about it anymore */
		if (w && h) {
			guint n, d;

			GST_DEBUG_OBJECT(transform, "dimensions already set to %dx%d, not fixating", w, h);
			if (!gst_value_is_fixed(to_par))
			{
				if (gst_video_calculate_display_ratio(&n, &d, from_w, from_h, from_par_n, from_par_d, w, h))
				{
					GST_DEBUG_OBJECT(transform, "fixating to_par to %dx%d", n, d);
					if (gst_structure_has_field(outs, "pixel-aspect-ratio"))
						gst_structure_fixate_field_nearest_fraction(outs, "pixel-aspect-ratio", n, d);
					else if (n != d)
						gst_structure_set(outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, n, d, NULL);
				}
			}
			goto done;
		}

		/* Calculate input DAR */
		if (!gst_util_fraction_multiply(from_w, from_h, from_par_n, from_par_d, &from_dar_n, &from_dar_d))
		{
			GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL),
					("Error calculating the output scaled size - integer overflow"));
			goto done;
		}

		GST_DEBUG_OBJECT(transform, "Input DAR is %d/%d", from_dar_n, from_dar_d);

		/* If either width or height are fixed there's not much we
		 * can do either except choosing a height or width and PAR
		 * that matches the DAR as good as possible
		 */
		if (h)
		{
			GstStructure *tmp;
			gint set_w, set_par_n, set_par_d;

			GST_DEBUG_OBJECT(transform, "height is fixed (%d)", h);

			/* If the PAR is fixed too, there's not much to do
			 * except choosing the width that is nearest to the
			 * width with the same DAR */
			if (gst_value_is_fixed(to_par))
			{
				to_par_n = gst_value_get_fraction_numerator(to_par);
				to_par_d = gst_value_get_fraction_denominator(to_par);

				GST_DEBUG_OBJECT(transform, "PAR is fixed %d/%d", to_par_n, to_par_d);

				if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, to_par_d, to_par_n, &num, &den))
				{
					GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
					goto done;
				}

				w = (guint) gst_util_uint64_scale_int(h, num, den);
				gst_structure_fixate_field_nearest_int(outs, "width", w);

				goto done;
			}

			/* The PAR is not fixed and it's quite likely that we can set
			 * an arbitrary PAR. */

			/* Check if we can keep the input width */
			tmp = gst_structure_copy(outs);
			gst_structure_fixate_field_nearest_int(tmp, "width", from_w);
			gst_structure_get_int(tmp, "width", &set_w);

			/* Might have failed but try to keep the DAR nonetheless by
			 * adjusting the PAR */
			if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, h, set_w, &to_par_n, &to_par_d))
			{
				GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
				gst_structure_free(tmp);
				goto done;
			}

			if (!gst_structure_has_field(tmp, "pixel-aspect-ratio"))
				gst_structure_set_value(tmp, "pixel-aspect-ratio", to_par);
			gst_structure_fixate_field_nearest_fraction(tmp, "pixel-aspect-ratio", to_par_n, to_par_d);
			gst_structure_get_fraction(tmp, "pixel-aspect-ratio", &set_par_n, &set_par_d);
			gst_structure_free(tmp);

			/* Check if the adjusted PAR is accepted */
			if (set_par_n == to_par_n && set_par_d == to_par_d)
			{
				if (gst_structure_has_field(outs, "pixel-aspect-ratio") || set_par_n != set_par_d)
					gst_structure_set(outs, "width", G_TYPE_INT, set_w, "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d, NULL);
				goto done;
			}

			/* Otherwise scale the width to the new PAR and check if the
			 * adjusted with is accepted. If all that fails we can't keep
			 * the DAR */
			if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, set_par_d, set_par_n, &num, &den))
			{
				GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
				goto done;
			}

			w = (guint) gst_util_uint64_scale_int(h, num, den);
			gst_structure_fixate_field_nearest_int(outs, "width", w);
			if (gst_structure_has_field(outs, "pixel-aspect-ratio") || set_par_n != set_par_d)
				gst_structure_set(outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d, NULL);

			goto done;
		}
		else if (w)
		{
			GstStructure *tmp;
			gint set_h, set_par_n, set_par_d;

			GST_DEBUG_OBJECT(transform, "width is fixed (%d)", w);

			/* If the PAR is fixed too, there's not much to do
			 * except choosing the height that is nearest to the
			 * height with the same DAR */
			if (gst_value_is_fixed(to_par))
			{
				to_par_n = gst_value_get_fraction_numerator(to_par);
				to_par_d = gst_value_get_fraction_denominator(to_par);

				GST_DEBUG_OBJECT(transform, "PAR is fixed %d/%d", to_par_n, to_par_d);

				if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, to_par_d, to_par_n, &num, &den))
				{
					GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
					goto done;
				}

				h = (guint) gst_util_uint64_scale_int(w, den, num);
				gst_structure_fixate_field_nearest_int(outs, "height", h);

				goto done;
			}

			/* The PAR is not fixed and it's quite likely that we can set
			 * an arbitrary PAR. */

			/* Check if we can keep the input height */
			tmp = gst_structure_copy(outs);
			gst_structure_fixate_field_nearest_int(tmp, "height", from_h);
			gst_structure_get_int(tmp, "height", &set_h);

			/* Might have failed but try to keep the DAR nonetheless by
			 * adjusting the PAR */
			if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, set_h, w, &to_par_n, &to_par_d))
			{
				GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
				gst_structure_free(tmp);
				goto done;
			}
			if (!gst_structure_has_field(tmp, "pixel-aspect-ratio"))
				gst_structure_set_value(tmp, "pixel-aspect-ratio", to_par);
			gst_structure_fixate_field_nearest_fraction(tmp, "pixel-aspect-ratio", to_par_n, to_par_d);
			gst_structure_get_fraction(tmp, "pixel-aspect-ratio", &set_par_n, &set_par_d);
			gst_structure_free(tmp);

			/* Check if the adjusted PAR is accepted */
			if (set_par_n == to_par_n && set_par_d == to_par_d)
			{
				if (gst_structure_has_field(outs, "pixel-aspect-ratio") || set_par_n != set_par_d)
					gst_structure_set(outs, "height", G_TYPE_INT, set_h,
							"pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d,
							NULL);
				goto done;
			}

			/* Otherwise scale the height to the new PAR and check if the
			 * adjusted with is accepted. If all that fails we can't keep
			 * the DAR */
			if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, set_par_d, set_par_n, &num, &den))
			{
				GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
				goto done;
			}

			h = (guint) gst_util_uint64_scale_int(w, den, num);
			gst_structure_fixate_field_nearest_int(outs, "height", h);
			if (gst_structure_has_field(outs, "pixel-aspect-ratio") || set_par_n != set_par_d)
				gst_structure_set(outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d, NULL);

			goto done;
		}
		else if (gst_value_is_fixed(to_par))
		{
			GstStructure *tmp;
			gint set_h, set_w, f_h, f_w;

			to_par_n = gst_value_get_fraction_numerator(to_par);
			to_par_d = gst_value_get_fraction_denominator(to_par);

			/* Calculate scale factor for the PAR change */
			if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, to_par_n, to_par_d, &num, &den))
			{
				GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
				goto done;
			}

			/* Try to keep the input height (because of interlacing) */
			tmp = gst_structure_copy(outs);
			gst_structure_fixate_field_nearest_int(tmp, "height", from_h);
			gst_structure_get_int(tmp, "height", &set_h);

			/* This might have failed but try to scale the width
			 * to keep the DAR nonetheless */
			w = (guint) gst_util_uint64_scale_int(set_h, num, den);
			gst_structure_fixate_field_nearest_int(tmp, "width", w);
			gst_structure_get_int(tmp, "width", &set_w);
			gst_structure_free(tmp);

			/* We kept the DAR and the height is nearest to the original height */
			if (set_w == w)
			{
				gst_structure_set(outs, "width", G_TYPE_INT, set_w, "height", G_TYPE_INT, set_h, NULL);
				goto done;
			}

			f_h = set_h;
			f_w = set_w;

			/* If the former failed, try to keep the input width at least */
			tmp = gst_structure_copy(outs);
			gst_structure_fixate_field_nearest_int(tmp, "width", from_w);
			gst_structure_get_int(tmp, "width", &set_w);

			/* This might have failed but try to scale the width
			 * to keep the DAR nonetheless */
			h = (guint) gst_util_uint64_scale_int(set_w, den, num);
			gst_structure_fixate_field_nearest_int(tmp, "height", h);
			gst_structure_get_int(tmp, "height", &set_h);
			gst_structure_free(tmp);

			/* We kept the DAR and the width is nearest to the original width */
			if (set_h == h)
			{
				gst_structure_set(outs, "width", G_TYPE_INT, set_w, "height", G_TYPE_INT, set_h, NULL);
				goto done;
			}

			/* If all this failed, keep the height that was nearest to the orignal
			 * height and the nearest possible width. This changes the DAR but
			 * there's not much else to do here.
			 */
			gst_structure_set(outs, "width", G_TYPE_INT, f_w, "height", G_TYPE_INT, f_h, NULL);
			goto done;
		}
		else
		{
			GstStructure *tmp;
			gint set_h, set_w, set_par_n, set_par_d, tmp2;

			/* width, height and PAR are not fixed but passthrough is not possible */

			/* First try to keep the height and width as good as possible
			 * and scale PAR */
			tmp = gst_structure_copy(outs);
			gst_structure_fixate_field_nearest_int(tmp, "height", from_h);
			gst_structure_get_int(tmp, "height", &set_h);
			gst_structure_fixate_field_nearest_int(tmp, "width", from_w);
			gst_structure_get_int(tmp, "width", &set_w);

			if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, set_h, set_w, &to_par_n, &to_par_d))
			{
				GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
				goto done;
			}

			if (!gst_structure_has_field(tmp, "pixel-aspect-ratio"))
				gst_structure_set_value(tmp, "pixel-aspect-ratio", to_par);
			gst_structure_fixate_field_nearest_fraction(tmp, "pixel-aspect-ratio", to_par_n, to_par_d);
			gst_structure_get_fraction(tmp, "pixel-aspect-ratio", &set_par_n, &set_par_d);
			gst_structure_free(tmp);

			if (set_par_n == to_par_n && set_par_d == to_par_d)
			{
				gst_structure_set(outs, "width", G_TYPE_INT, set_w, "height", G_TYPE_INT, set_h, NULL);

				if (gst_structure_has_field(outs, "pixel-aspect-ratio") || set_par_n != set_par_d)
					gst_structure_set(outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d, NULL);
				goto done;
			}

			/* Otherwise try to scale width to keep the DAR with the set
			 * PAR and height */
			if (!gst_util_fraction_multiply(from_dar_n, from_dar_d, set_par_d, set_par_n, &num, &den))
			{
				GST_ELEMENT_ERROR(transform, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
				goto done;
			}

			w = (guint) gst_util_uint64_scale_int(set_h, num, den);
			tmp = gst_structure_copy(outs);
			gst_structure_fixate_field_nearest_int(tmp, "width", w);
			gst_structure_get_int(tmp, "width", &tmp2);
			gst_structure_free(tmp);

			if (tmp2 == w)
			{
				gst_structure_set(outs, "width", G_TYPE_INT, tmp2, "height", G_TYPE_INT, set_h, NULL);
				if (gst_structure_has_field(outs, "pixel-aspect-ratio") || set_par_n != set_par_d)
					gst_structure_set(outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d, NULL);
				goto done;
			}

			/* ... or try the same with the height */
			h = (guint) gst_util_uint64_scale_int(set_w, den, num);
			tmp = gst_structure_copy(outs);
			gst_structure_fixate_field_nearest_int(tmp, "height", h);
			gst_structure_get_int(tmp, "height", &tmp2);
			gst_structure_free(tmp);

			if (tmp2 == h)
			{
				gst_structure_set(outs, "width", G_TYPE_INT, set_w, "height", G_TYPE_INT, tmp2, NULL);
				if (gst_structure_has_field(outs, "pixel-aspect-ratio") || set_par_n != set_par_d)
					gst_structure_set(outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d, NULL);
				goto done;
			}

			/* If all fails we can't keep the DAR and take the nearest values
			 * for everything from the first try */
			gst_structure_set(outs, "width", G_TYPE_INT, set_w, "height", G_TYPE_INT, set_h, NULL);
			if (gst_structure_has_field(outs, "pixel-aspect-ratio") || set_par_n != set_par_d)
				gst_structure_set(outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d, NULL);
		}
	}

done:
	gst_imx_ipu_video_transform_fixate_format_caps(transform, caps, othercaps);

	GST_DEBUG_OBJECT(transform, "fixated othercaps to %" GST_PTR_FORMAT, (gpointer)othercaps);

	if (from_par == &fpar)
		g_value_unset(&fpar);
	if (to_par == &tpar)
		g_value_unset(&tpar);

	return othercaps;
}


#define SCORE_PALETTE_LOSS        1
#define SCORE_COLOR_LOSS          2
#define SCORE_ALPHA_LOSS          4
#define SCORE_CHROMA_W_LOSS       8
#define SCORE_CHROMA_H_LOSS      16
#define SCORE_DEPTH_LOSS         32

#define COLOR_MASK   (GST_VIDEO_FORMAT_FLAG_YUV | \
                      GST_VIDEO_FORMAT_FLAG_RGB | GST_VIDEO_FORMAT_FLAG_GRAY)
#define ALPHA_MASK   (GST_VIDEO_FORMAT_FLAG_ALPHA)
#define PALETTE_MASK (GST_VIDEO_FORMAT_FLAG_PALETTE)


/* calculate how much loss a conversion would be */
static void score_value(GstBaseTransform * base, const GstVideoFormatInfo * in_info, const GValue * val, gint * min_loss, const GstVideoFormatInfo ** out_info)
{
	const gchar *fname;
	const GstVideoFormatInfo *t_info;
	GstVideoFormatFlags in_flags, t_flags;
	gint loss;

	fname = g_value_get_string(val);
	t_info = gst_video_format_get_info(gst_video_format_from_string(fname));
	if (!t_info)
		return;

	/* accept input format immediately without loss */
	if (in_info == t_info)
	{
		*min_loss = 0;
		*out_info = t_info;
		return;
	}

	loss = 1;

	in_flags = GST_VIDEO_FORMAT_INFO_FLAGS(in_info);
	in_flags &= ~GST_VIDEO_FORMAT_FLAG_LE;
	in_flags &= ~GST_VIDEO_FORMAT_FLAG_COMPLEX;
	in_flags &= ~GST_VIDEO_FORMAT_FLAG_UNPACK;

	t_flags = GST_VIDEO_FORMAT_INFO_FLAGS(t_info);
	t_flags &= ~GST_VIDEO_FORMAT_FLAG_LE;
	t_flags &= ~GST_VIDEO_FORMAT_FLAG_COMPLEX;
	t_flags &= ~GST_VIDEO_FORMAT_FLAG_UNPACK;

	if ((t_flags & PALETTE_MASK) != (in_flags & PALETTE_MASK))
		loss += SCORE_PALETTE_LOSS;

	if ((t_flags & COLOR_MASK) != (in_flags & COLOR_MASK))
		loss += SCORE_COLOR_LOSS;

	if ((t_flags & ALPHA_MASK) != (in_flags & ALPHA_MASK))
		loss += SCORE_ALPHA_LOSS;

	if ((in_info->h_sub[1]) < (t_info->h_sub[1]))
		loss += SCORE_CHROMA_H_LOSS;
	if ((in_info->w_sub[1]) < (t_info->w_sub[1]))
		loss += SCORE_CHROMA_W_LOSS;

	if ((in_info->bits) > (t_info->bits))
		loss += SCORE_DEPTH_LOSS;

	GST_DEBUG_OBJECT(base, "score %s -> %s = %d",
			GST_VIDEO_FORMAT_INFO_NAME(in_info),
			GST_VIDEO_FORMAT_INFO_NAME(t_info), loss);

	if (loss < *min_loss)
	{
		GST_DEBUG_OBJECT(base, "found new best %d", loss);
		*out_info = t_info;
		*min_loss = loss;
	}
}


static void gst_imx_ipu_video_transform_fixate_format_caps(GstBaseTransform *transform, GstCaps *caps, GstCaps *othercaps)
{
	GstStructure *ins, *outs;
	const gchar *in_format;
	const GstVideoFormatInfo *in_info, *out_info = NULL;
	gint min_loss = G_MAXINT;
	guint i, capslen;

	ins = gst_caps_get_structure(caps, 0);
	in_format = gst_structure_get_string(ins, "format");
	if (!in_format)
		return;

	GST_DEBUG_OBJECT(transform, "source format %s", in_format);

	in_info = gst_video_format_get_info(gst_video_format_from_string(in_format));
	if (!in_info)
		return;

	outs = gst_caps_get_structure(othercaps, 0);

	capslen = gst_caps_get_size(othercaps);
	GST_DEBUG_OBJECT(transform, "iterate %d structures", capslen);
	for (i = 0; i < capslen; i++)
	{
		GstStructure *tests;
		const GValue *format;

		tests = gst_caps_get_structure(othercaps, i);
		format = gst_structure_get_value(tests, "format");
		/* should not happen */
		if (format == NULL)
			continue;

		if (GST_VALUE_HOLDS_LIST(format))
		{
			gint j, len;

			len = gst_value_list_get_size(format);
			GST_DEBUG_OBJECT(transform, "have %d formats", len);
			for (j = 0; j < len; j++)
			{
				const GValue *val;

				val = gst_value_list_get_value(format, j);
				if (G_VALUE_HOLDS_STRING(val))
				{
					score_value(transform, in_info, val, &min_loss, &out_info);
					if (min_loss == 0)
						break;
				}
			}
		} else if (G_VALUE_HOLDS_STRING(format))
			score_value(transform, in_info, format, &min_loss, &out_info);
	}
	if (out_info)
		gst_structure_set(outs, "format", G_TYPE_STRING, GST_VIDEO_FORMAT_INFO_NAME(out_info), NULL);
}


static gboolean gst_imx_ipu_video_transform_propose_allocation(GstBaseTransform *transform, G_GNUC_UNUSED GstQuery *decide_query, GstQuery *query)
{
	return gst_pad_peer_query(GST_BASE_TRANSFORM_SRC_PAD(transform), query);
}


static gboolean gst_imx_ipu_video_transform_decide_allocation(GstBaseTransform *transform, GstQuery *query)
{
	GstImxIpuVideoTransform *ipu_video_transform = GST_IMX_IPU_VIDEO_TRANSFORM(transform);
	GstCaps *outcaps;
	GstBufferPool *pool = NULL;
	guint size, min = 0, max = 0;
	GstStructure *config;
	GstVideoInfo vinfo;
	gboolean update_pool;

	gst_query_parse_allocation(query, &outcaps, NULL);
	gst_video_info_init(&vinfo);
	gst_video_info_from_caps(&vinfo, outcaps);

	GST_DEBUG_OBJECT(ipu_video_transform, "num allocation pools: %d", gst_query_get_n_allocation_pools(query));

	/* Look for an allocator which can allocate physical memory buffers */
	if (gst_query_get_n_allocation_pools(query) > 0)
	{
		for (guint i = 0; i < gst_query_get_n_allocation_pools(query); ++i)
		{
			gst_query_parse_nth_allocation_pool(query, i, &pool, &size, &min, &max);
			if (gst_buffer_pool_has_option(pool, GST_BUFFER_POOL_OPTION_IMX_PHYS_MEM))
				break;
		}

		size = MAX(size, vinfo.size);
		update_pool = TRUE;
	}
	else
	{
		pool = NULL;
		size = vinfo.size;
		min = max = 0;
		update_pool = FALSE;
	}

	/* Either no pool or no pool with the ability to allocate physical memory buffers
	 * has been found -> create a new pool */
	if ((pool == NULL) || !gst_buffer_pool_has_option(pool, GST_BUFFER_POOL_OPTION_IMX_PHYS_MEM))
	{
		if (pool == NULL)
			GST_DEBUG_OBJECT(ipu_video_transform, "no pool present; creating new pool");
		else
			GST_DEBUG_OBJECT(ipu_video_transform, "no pool supports physical memory buffers; creating new pool");
		pool = gst_imx_ipu_blitter_create_bufferpool(ipu_video_transform->priv->blitter, outcaps, size, min, max, NULL, NULL);
	}
	else
	{
		config = gst_buffer_pool_get_config(pool);
		gst_buffer_pool_config_set_params(config, outcaps, size, min, max);
		gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_IMX_PHYS_MEM);
		gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
		gst_buffer_pool_set_config(pool, config);
	}

	GST_DEBUG_OBJECT(
		ipu_video_transform,
		"pool config:  outcaps: %" GST_PTR_FORMAT "  size: %u  min buffers: %u  max buffers: %u",
		(gpointer)outcaps,
		size,
		min,
		max
	);

	if (update_pool)
		gst_query_set_nth_allocation_pool(query, 0, pool, size, min, max);
	else
		gst_query_add_allocation_pool(query, pool, size, min, max);

	if (pool != NULL)
		gst_object_unref(pool);

	return TRUE;
}


static gboolean gst_imx_ipu_video_transform_set_info(GstVideoFilter *filter, GstCaps *in, GstVideoInfo *in_info, GstCaps *out, GstVideoInfo *out_info)
{
	GstImxIpuVideoTransform *ipu_video_transform = GST_IMX_IPU_VIDEO_TRANSFORM(filter);

	/* For IPU blitting operations, equality is limited to width, height, pixelformat */
	ipu_video_transform->inout_caps_equal =
		(in_info->width == out_info->width) &&
		(in_info->height == out_info->height) &&
		(in_info->finfo->format == out_info->finfo->format)
		;

	if (ipu_video_transform->inout_caps_equal)
		GST_DEBUG_OBJECT(filter, "input and output caps are equal");
	else
		GST_DEBUG_OBJECT(filter, "input and output caps are not equal:  input: %" GST_PTR_FORMAT "  output: %" GST_PTR_FORMAT, (gpointer)in, (gpointer)out);

	gst_imx_ipu_blitter_set_input_info(ipu_video_transform->priv->blitter, in_info);

	return TRUE;
}


static GstFlowReturn gst_imx_ipu_video_transform_prepare_output_buffer(GstBaseTransform *trans, GstBuffer *input, GstBuffer **outbuf)
{
	gboolean passthrough = FALSE;
	GstImxIpuVideoTransform *ipu_video_transform = GST_IMX_IPU_VIDEO_TRANSFORM(trans);

	LOCK_BLITTER_MUTEX(ipu_video_transform);

	/* Test if passthrough should be enabled */
	if ((input != NULL) && ipu_video_transform->inout_caps_equal)
	{
		/* if there is an input buffer and the input/output caps are equal,
		 * assume passthrough should be used, and test for exceptions where
		 * passthrough must not be enabled */
		passthrough = TRUE;

		/* Aforementioned exceptions are transforms like rotation, deinterlacing ... */
		if (gst_imx_ipu_blitter_are_transforms_enabled(ipu_video_transform->priv->blitter))
		{
			GstVideoMeta *video_meta = gst_buffer_get_video_meta(input);

			/* Try to find video metadata; if found, check if the interlacing
			 * flag is set. If deinterlacing is enabled, and this flag is set,
			 * then deinterlacing must be performed, therefore passthrough must
			 * not be enabled. */
			if (video_meta != NULL)
			{
				if (gst_imx_ipu_blitter_get_deinterlace_mode(ipu_video_transform->priv->blitter) != GST_IMX_IPU_BLITTER_DEINTERLACE_NONE)
				{
					switch (ipu_video_transform->priv->blitter->input_video_info.interlace_mode)
					{
						gboolean flag_found;
						case GST_VIDEO_INTERLACE_MODE_INTERLEAVED:
							GST_LOG_OBJECT(trans, "interlacing in interleaved mode");
							passthrough = FALSE;
							break;
						case GST_VIDEO_INTERLACE_MODE_MIXED:
							flag_found = ((video_meta->flags & GST_VIDEO_FRAME_FLAG_INTERLACED) == 0);
							passthrough = passthrough && !flag_found;
							GST_LOG_OBJECT(trans, "interlacing in mixed mode - flag %s", flag_found ? "found" : "not found");
							break;
						case GST_VIDEO_INTERLACE_MODE_PROGRESSIVE:
						case GST_VIDEO_INTERLACE_MODE_FIELDS:
							break;
						default:
							break;
					}
				}
			}
			else
				GST_LOG_OBJECT(trans, "no video metadata found");

			/* Check for rotation. If rotation shall be applied, passthrough
			 * is not possible. */
			if (gst_imx_ipu_blitter_get_output_rotation_mode(ipu_video_transform->priv->blitter) != GST_IMX_IPU_BLITTER_ROTATION_NONE)
			{
				GST_LOG_OBJECT(trans, "rotation requested");
				passthrough = FALSE;
			}

			/* Check for effective cropping. Effective means the crop rectangle
			 * is not exactly the same as the input. (Crop rectangle 0,0,picwidth,picheight
			 * doesnt actually crop anything.) If there is an effective crop rectangle,
			 * disable passthrough. */
			if (gst_imx_ipu_blitter_is_crop_enabled(ipu_video_transform->priv->blitter))
			{
				GstVideoCropMeta *video_crop_meta = gst_buffer_get_video_crop_meta(input);
				if (video_crop_meta != NULL)
				{
					if (video_meta != NULL)
					{
						if (
							(video_crop_meta->x != 0) ||
							(video_crop_meta->y != 0) ||
							(video_crop_meta->width != video_meta->width) ||
							(video_crop_meta->height != video_meta->height)
						)
							passthrough = FALSE;

						GST_LOG_OBJECT(trans, "effective crop rectangle found: %s", passthrough ? "yes" : "no");
					}
				}
			}
				GST_LOG_OBJECT(trans, "no video crop metadata found");
		}
	}
	else if (!ipu_video_transform->inout_caps_equal)
		GST_LOG_OBJECT(trans, "input and output caps are not equal");
	else if (input == NULL)
		GST_LOG_OBJECT(trans, "input buffer is NULL");

	UNLOCK_BLITTER_MUTEX(ipu_video_transform);

	GST_LOG_OBJECT(trans, "passthrough: %s", passthrough ? "yes" : "no");
	gst_base_transform_set_passthrough(trans, passthrough);

	return GST_BASE_TRANSFORM_CLASS(gst_imx_ipu_video_transform_parent_class)->prepare_output_buffer(trans, input, outbuf);
}


static GstFlowReturn gst_imx_ipu_video_transform_transform_frame(GstVideoFilter *filter, GstVideoFrame *in, GstVideoFrame *out)
{
	gboolean ret;
	GstImxIpuVideoTransform *ipu_video_transform = GST_IMX_IPU_VIDEO_TRANSFORM(filter);

	LOCK_BLITTER_MUTEX(ipu_video_transform);

	/* using early exit optimization here to avoid calls if necessary */
	ret = TRUE;
	ret = ret && gst_imx_ipu_blitter_set_input_buffer(ipu_video_transform->priv->blitter, in->buffer);
	ret = ret && gst_imx_ipu_blitter_set_output_buffer(ipu_video_transform->priv->blitter, out->buffer);
	ret = ret && gst_imx_ipu_blitter_blit(ipu_video_transform->priv->blitter);

	UNLOCK_BLITTER_MUTEX(ipu_video_transform);

	return ret ? GST_FLOW_OK : GST_FLOW_ERROR;
}

