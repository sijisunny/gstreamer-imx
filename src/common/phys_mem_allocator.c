/* Common allocator for allocating physical memory
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
#include "phys_mem_allocator.h"


GST_DEBUG_CATEGORY_STATIC(imx_phys_mem_allocator_debug);
#define GST_CAT_DEFAULT imx_phys_mem_allocator_debug


static void gst_imx_phys_mem_allocator_finalize(GObject *object);
static GstMemory* gst_imx_phys_mem_allocator_alloc(GstAllocator *allocator, gsize size, GstAllocationParams *params);
static void gst_imx_phys_mem_allocator_free(GstAllocator *allocator, GstMemory *memory);
static gpointer gst_imx_phys_mem_allocator_map(GstMemory *mem, gsize maxsize, GstMapFlags flags);
static void gst_imx_phys_mem_allocator_unmap(GstMemory *mem);
static GstMemory* gst_imx_phys_mem_allocator_copy(GstMemory *mem, gssize offset, gssize size);
static GstMemory* gst_imx_phys_mem_allocator_share(GstMemory *mem, gssize offset, gssize size);
static gboolean gst_imx_phys_mem_allocator_is_span(GstMemory *mem1, GstMemory *mem2, gsize *offset);


G_DEFINE_ABSTRACT_TYPE(GstImxPhysMemAllocator, gst_imx_phys_mem_allocator, GST_TYPE_ALLOCATOR)


void gst_imx_phys_mem_allocator_class_init(GstImxPhysMemAllocatorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GstAllocatorClass *parent_class = GST_ALLOCATOR_CLASS(klass);

	GST_DEBUG_CATEGORY_INIT(imx_phys_mem_allocator_debug, "imxphysmemallocator", 0, "Allocator for physically contiguous memory blocks");

	klass->alloc_phys_mem  = NULL;
	klass->free_phys_mem   = NULL;
	klass->map_phys_mem    = NULL;
	klass->unmap_phys_mem  = NULL;
	parent_class->alloc    = GST_DEBUG_FUNCPTR(gst_imx_phys_mem_allocator_alloc);
	parent_class->free     = GST_DEBUG_FUNCPTR(gst_imx_phys_mem_allocator_free);
	object_class->finalize = GST_DEBUG_FUNCPTR(gst_imx_phys_mem_allocator_finalize);
}


void gst_imx_phys_mem_allocator_init(GstImxPhysMemAllocator *allocator)
{
	GstAllocator *parent = GST_ALLOCATOR(allocator);


	parent->mem_type    = NULL;
	parent->mem_map     = GST_DEBUG_FUNCPTR(gst_imx_phys_mem_allocator_map);
	parent->mem_unmap   = GST_DEBUG_FUNCPTR(gst_imx_phys_mem_allocator_unmap);
	parent->mem_copy    = GST_DEBUG_FUNCPTR(gst_imx_phys_mem_allocator_copy);
	parent->mem_share   = GST_DEBUG_FUNCPTR(gst_imx_phys_mem_allocator_share);
	parent->mem_is_span = GST_DEBUG_FUNCPTR(gst_imx_phys_mem_allocator_is_span);
}


static void gst_imx_phys_mem_allocator_finalize(GObject *object)
{
	GST_INFO_OBJECT(object, "shutting down physical memory allocator");
	G_OBJECT_CLASS (gst_imx_phys_mem_allocator_parent_class)->finalize(object);
}


static GstImxPhysMemory* gst_imx_phys_mem_new_internal(GstImxPhysMemAllocator *phys_mem_alloc, GstMemory *parent, gsize maxsize, GstMemoryFlags flags, gsize align, gsize offset, gsize size)
{
	GstImxPhysMemory *phys_mem;
	phys_mem = g_slice_alloc(sizeof(GstImxPhysMemory));
	phys_mem->mapped_virt_addr = NULL;
	phys_mem->phys_addr = 0;
	phys_mem->cpu_addr = 0;

	gst_memory_init(GST_MEMORY_CAST(phys_mem), flags, GST_ALLOCATOR_CAST(phys_mem_alloc), parent, maxsize, align, offset, size);

	return phys_mem;
}


static GstImxPhysMemory* gst_imx_phys_mem_allocator_alloc_internal(GstAllocator *allocator, GstMemory *parent, gsize maxsize, GstMemoryFlags flags, gsize align, gsize offset, gsize size)
{
	GstImxPhysMemAllocator *phys_mem_alloc;
	GstImxPhysMemAllocatorClass *klass;
	GstImxPhysMemory *phys_mem;

	phys_mem_alloc = GST_IMX_PHYS_MEM_ALLOCATOR(allocator);
	klass = GST_IMX_PHYS_MEM_ALLOCATOR_CLASS(G_OBJECT_GET_CLASS(allocator));

	GST_INFO_OBJECT(
		allocator,
		"alloc_internal called: maxsize: %u, align: %u, offset: %u, size: %u",
		maxsize,
		align,
		offset,
		size
	);

	phys_mem = gst_imx_phys_mem_new_internal(phys_mem_alloc, parent, maxsize, flags, align, offset, size);
	if (!klass->alloc_phys_mem(phys_mem_alloc, phys_mem, maxsize))
	{
		g_slice_free1(sizeof(GstImxPhysMemory), phys_mem);
		return NULL;
	}

	if ((offset > 0) && (flags & GST_MEMORY_FLAG_ZERO_PREFIXED))
	{
		gpointer ptr = klass->map_phys_mem(phys_mem_alloc, phys_mem, maxsize, GST_MAP_WRITE);
		memset(ptr, 0, offset);
		klass->unmap_phys_mem(phys_mem_alloc, phys_mem);
	}

	return phys_mem;
}


static GstMemory* gst_imx_phys_mem_allocator_alloc(GstAllocator *allocator, gsize size, GstAllocationParams *params)
{
	gsize maxsize;
	GstImxPhysMemory *phys_mem;

	maxsize = size + params->prefix + params->padding;
	phys_mem = gst_imx_phys_mem_allocator_alloc_internal(allocator, NULL, maxsize, params->flags, params->align, params->prefix, size);

	if (phys_mem != NULL)
		GST_INFO_OBJECT(allocator, "allocated memory block %p at phys addr 0x%x with %u bytes", (gpointer)phys_mem, phys_mem->phys_addr, size);
	else
		GST_WARNING_OBJECT(allocator, "could not allocate memory block with %u bytes", size);

	return (GstMemory *)phys_mem;
}


static void gst_imx_phys_mem_allocator_free(GstAllocator *allocator, GstMemory *memory)
{
	GstImxPhysMemory *phys_mem = (GstImxPhysMemory *)memory;
	GstImxPhysMemAllocator *phys_mem_alloc = GST_IMX_PHYS_MEM_ALLOCATOR(allocator);
	GstImxPhysMemAllocatorClass *klass = GST_IMX_PHYS_MEM_ALLOCATOR_CLASS(G_OBJECT_GET_CLASS(allocator));

	klass->free_phys_mem(phys_mem_alloc, phys_mem);

	GST_INFO_OBJECT(allocator, "freed block %p at phys addr 0x%x with size: %u", (gpointer)memory, phys_mem->phys_addr, memory->size);
}


static gpointer gst_imx_phys_mem_allocator_map(GstMemory *mem, gsize maxsize, GstMapFlags flags)
{
	GstImxPhysMemory *phys_mem = (GstImxPhysMemory *)mem;
	GstImxPhysMemAllocator *phys_mem_alloc = GST_IMX_PHYS_MEM_ALLOCATOR(mem->allocator);
	GstImxPhysMemAllocatorClass *klass = GST_IMX_PHYS_MEM_ALLOCATOR_CLASS(G_OBJECT_GET_CLASS(mem->allocator));

	GST_TRACE_OBJECT(phys_mem_alloc, "mapping %u bytes from memory block %p (phys addr %p)", maxsize, (gpointer)mem, (gpointer)(phys_mem->phys_addr));

	return klass->map_phys_mem(phys_mem_alloc, phys_mem, maxsize, flags);
}


static void gst_imx_phys_mem_allocator_unmap(GstMemory *mem)
{
	GstImxPhysMemory *phys_mem = (GstImxPhysMemory *)mem;
	GstImxPhysMemAllocator *phys_mem_alloc = GST_IMX_PHYS_MEM_ALLOCATOR(mem->allocator);
	GstImxPhysMemAllocatorClass *klass = GST_IMX_PHYS_MEM_ALLOCATOR_CLASS(G_OBJECT_GET_CLASS(mem->allocator));

	GST_TRACE_OBJECT(phys_mem_alloc, "unmapping memory block %p (phys addr %p)", (gpointer)mem, (gpointer)(phys_mem->phys_addr));

	klass->unmap_phys_mem(phys_mem_alloc, phys_mem);
}


static GstMemory* gst_imx_phys_mem_allocator_copy(GstMemory *mem, gssize offset, gssize size)
{
	GstImxPhysMemory *copy;

	if (size == -1)
		size = ((gssize)(mem->size) > offset) ? (mem->size - offset) : 0;

	copy = gst_imx_phys_mem_allocator_alloc_internal(mem->allocator, NULL, mem->maxsize, 0, mem->align, mem->offset + offset, size);

	{
		gpointer srcptr, destptr;
		GstImxPhysMemAllocator *phys_mem_alloc = (GstImxPhysMemAllocator*)(mem->allocator);
		GstImxPhysMemAllocatorClass *klass = GST_IMX_PHYS_MEM_ALLOCATOR_CLASS(G_OBJECT_GET_CLASS(mem->allocator));

		srcptr = klass->map_phys_mem(phys_mem_alloc, (GstImxPhysMemory *)mem, mem->maxsize, GST_MAP_READ);
		destptr = klass->map_phys_mem(phys_mem_alloc, copy, mem->maxsize, GST_MAP_WRITE);

		memcpy(destptr, srcptr, mem->maxsize);

		klass->unmap_phys_mem(phys_mem_alloc, copy);
		klass->unmap_phys_mem(phys_mem_alloc, (GstImxPhysMemory *)mem);
	}

	GST_INFO_OBJECT(
		mem->allocator,
		"copied block %p, new copied block %p; offset: %d, size: %d; source block maxsize: %u, align: %u, offset: %u, size: %u",
		(gpointer)mem, (gpointer)copy,
		offset,
		size,
		mem->maxsize,
		mem->align,
		mem->offset,
		mem->size
	);

	return (GstMemory *)copy;
}


static GstMemory* gst_imx_phys_mem_allocator_share(GstMemory *mem, gssize offset, gssize size)
{
	GstImxPhysMemory *phys_mem;
	GstImxPhysMemory *sub;
	GstMemory *parent;

	phys_mem = (GstImxPhysMemory *)mem;

	if (size == -1)
		size = ((gssize)(phys_mem->mem.size) > offset) ? (phys_mem->mem.size - offset) : 0;

	if ((parent = phys_mem->mem.parent) == NULL)
		parent = (GstMemory *)mem;

	sub = gst_imx_phys_mem_new_internal(
		GST_IMX_PHYS_MEM_ALLOCATOR(phys_mem->mem.allocator),
		parent,
		phys_mem->mem.maxsize,
		GST_MINI_OBJECT_FLAGS(parent) | GST_MINI_OBJECT_FLAG_LOCK_READONLY,
		phys_mem->mem.align,
		phys_mem->mem.offset + offset,
		size
	);
	sub->phys_addr = phys_mem->phys_addr;
	sub->cpu_addr = phys_mem->cpu_addr;

	GST_INFO_OBJECT(
		mem->allocator,
		"shared block %p, new sub block %p; offset: %d, size: %d; source block maxsize: %u, align: %u, offset: %u, size: %u",
		(gpointer)mem, (gpointer)sub,
		offset,
		size,
		mem->maxsize,
		mem->align,
		mem->offset,
		mem->size
	);

	return (GstMemory *)sub;
}


static gboolean gst_imx_phys_mem_allocator_is_span(G_GNUC_UNUSED GstMemory *mem1, G_GNUC_UNUSED GstMemory *mem2, G_GNUC_UNUSED gsize *offset)
{
	return FALSE;
}


guintptr gst_imx_phys_memory_get_phys_addr(GstMemory *mem)
{
	return ((GstImxPhysMemory *)mem)->phys_addr;
}


guintptr gst_imx_phys_memory_get_cpu_addr(GstMemory *mem)
{
	return ((GstImxPhysMemory *)mem)->cpu_addr;
}


gboolean gst_imx_is_phys_memory(GstMemory *mem)
{
	return GST_IS_IMX_PHYS_MEM_ALLOCATOR(mem->allocator);
}


