# GL Renderer Cleanup (wip/chergert/glproto)

While spending a lot of time on the macOS backend, I often found myself
confused while reading the GL renderer. There are a lot of moving parts and
it grew organically, so being OpenGL illterate that isn't very surprising.

However, the GL renderer is pretty important for us as it serves as a base
for how other renderers should work. We need to be pretty hygenic there
so that new renderers can follow. I would really like to be able to point
someone at it as an example for writing, for example, a Metal renderer.

Additionally, the OpenGL performance on macOS is not very good and we rarely
hit 60fps for situations that are just fine on Linux. We have to be extra
careful with such piss-poor OpenGL drivers.

## Goals

 * Clearer abstractions, fix some incorrect terminology that I found
   confusing when learning about the related bits in OpenGL.
 * Move in a direction that could allow threaded rendering soon.
 * Perform fewer unnecessary uniform changes.
 * Support for additional batch merging and out-of-order batching. 
 * Additional debugging capabilities including discovering overwrites
   and further sysprof integration.

## Current issues (incomplete porting)

 * Texture slices are not yet implemented
 * Texture atlases are designed but not yet implemented
 * Programs are not currently shared (this doesn't actually seem
   to be guaranteed anyway by OpenGL, but *works* everywhere?)
 * A few nodes still need to be implemented
 * Handful of coordinate issues to finish flushing out from porting
   the draw operations from gskglrenderer.

## Design

### Program Definitions (gskglprograms.defs)

You can now define programs using XMacros in gskglprograms.defs. This vastly
simplifies the process of getting up and running with a new program. Diffs
should be short and compact should we add new node types.

### GskGLCommandQueue (gskglcommandqueue.c)

This object handles the majority of actually GL operations. Everything else
wraps this object in some way. Contrast this to the current renderer where
some state is tracked in various places and is not necessarily in sync.

It tracks attachments (FBOs and Textures) so that it can minimally change
attachment state as batches are executed. See GskGLAttachmentState.

It also tracks uniform state across all programs so that we can minimally
update uniforms as we go. We vaguely did this before, but a lot fell through
the cracks due to how ops_set_program() worked.

Draws now have a begin/end call to make it easier to have a point where we
can do batch creation/merging. When finishing a batch, we can chain it to
the previous batch or alternatively move it out-of-order if we do not have
any drawing operations that overlap between the two calls. Much work here is
to be done, but this lays the groundwork to actually be able to do it.

Eventually, we would like to be able to snapshot the command queue and pass
it off to another thread for processing (possibly via extensions to
GdkGLContext).

### GskGLDriver (gskgldriver.c)

The driver is the primary frontend to a lot of the machinery here. It manages
the command queue, access to programs, creation and destruction of textures
or render targets, as well as temporary pooling of textures during the
generation of command batches.

### GskGLProgram

This abstraction manages a specific program. It also maps an internal
enum (UNIFORM_*) which we can use throughout code to the uniform location
on the GPU.

We use it to begin/end draw calls as it can update the necessary uniforms
based on the changing state in GskGLRenderJob.

### GskGLRenderJob

Much of the work of translating nodes into draw commands (through the
GskGLProgram and GskGLCommandQueue) is done from GskGLRenderJob. It helps
keep that work focused and separate from other mechanics that are part of
the renderer itself.

Some work has been done to simplify the abstractions for offscreen drawing.
Tracking issues with this previously was quite difficult and the source of a
lot of indirection. Some might be a bit more verbose now, but much clearer
as a result.

### GskGLTextureLibrary (IconLibrary, GlyphLibrary, ShadowLibrary, etc)

This base class deals with texture atlases through the GskGLDriver. All of
our texture libraries build upon this.

We avoid the word cache here because it's too overloaded giving that we
cache textures from some render nodes.

### GskGLBuffer

This is a fairly generic buffer that can be uploaded to the GPU. In general
it is only used for the VBO to upload vertices as used by the command queue.
It's possible, though, that we could re-use it for UBO's in the future if we
decide to attempt to reduce our number of glDrawArrays() calls by sending
everything to the GPU up-front.

Additionally, it is very easy to start double-buffering VBOs now instead of
creating/throwing-away on each frame should we find that helps performance.
For now, we do not double or tripple buffer.

### GskGLCompiler

Some work was done to simplify shader compilers with an API that is a bit
easier to use from XMacros.




