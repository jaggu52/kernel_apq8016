Linux 5.15

DRM internals

* Every DRM driver uses a struct drm_driver and passes it to
  drm_dev_alloc().
  The struct drm_driver describes supported features and pointers to
  methods the DRM core calls to implement the DRM API.
  Driver information includes major, minor and patchlevel, name,
  description, and date.

Resource: https://www.kernel.org/doc/html/v5.15/gpu/drm-internals.html#driver-initialization


* A device instance for a DRM driver is represented by struct drm_device.
  This is allocated and initialized with devm_drm_dev_alloc().
  The driver then needs to initialize subsystems such as memory
  management, vblank handling, modesetting support, initial output
  configuration, and the corresponding hardware. Finally, when
  everything is up and running and ready for userspace, the device
  instance can be published using drm_dev_register().

* When cleaning up a device instance, everything needs to be done in
  reverse. First unpublish the device instance with
  drm_dev_unregister(). Then clean up any other resources allocated at
  device initialization and drop the driver's reference to drm_device
  using drm_dev_put().

Todo
drm_dev_put()?
drmm_add_action(), drmm_kmalloc() ?
Devres-managed resources like devm_kmalloc() can only be used for
resources directly related to the underlying hardware device, and only
in code paths fully protected by drm_dev_enter() and drm_dev_exit().

Resource: https://www.kernel.org/doc/html/v5.15/gpu/drm-internals.html#device-instance-and-driver-handling

* Basic driver skeleton
https://www.kernel.org/doc/html/v5.15/gpu/drm-internals.html#display-driver-example

struct drm_device {
	/** @mode_config: Current mode config */
	struct drm_mode_config mode_config;

struct drm_mode_config
 *Mode configuration control structure
 * Core mode resource tracking structure.  All CRTC, encoders, and connectors
   enumerated by the driver are added here, as are global properties

struct drm_mode_config_helper_funcs {
 * global modeset helper operations. These helper functions are used by the atomic helpers.
 * Helpers are extension of drm core provided by driver. Driver specific operation


struct drm_mode_config_funcs {
/**
 * struct drm_mode_config_funcs - basic driver provided mode setting functions
 * Some global (i.e. not per-CRTC, connector, etc) mode setting functions that
 * involve drivers.
 */
global mode setting functions, fb_create, atomic_commit/check, get_format_info.
functions to setting all crtc, plane, connector, encoder is involved
	
drmm_mode_config_init();
 * Initialize @dev's mode_config structure, used for tracking the graphics
  configuration of @dev. This initializes the modeset locks

drm_mode_config_reset(drm);

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
DRM CORE FUNCS vs HELPER FUNCS (COPY-PASTE NOTES)
===============================================

BIG PICTURE
-----------
Core funcs and helper funcs serve different roles in DRM/KMS.

- Core funcs   : Mandatory driver ↔ DRM core contract
- Helper funcs : Optional shared logic used by DRM helpers

If core funcs are missing → driver will NOT work.
If helper funcs are missing → driver must implement everything manually.


CALL FLOW OVERVIEW
------------------
userspace (modetest / weston / kmscube)
        |
        v
DRM core  ── calls ──► core funcs (driver must implement)
        |
        v
DRM helpers ── call ──► helper funcs (optional convenience layer)


CORE FUNCS (MANDATORY)
---------------------
Core funcs are called DIRECTLY by the DRM core.
They define WHAT the hardware can do.

Global core funcs:
- struct drm_mode_config_funcs
  - fb_create
  - atomic_check
  - atomic_commit
  - output_poll_changed

Registered as:
  dev->mode_config.funcs = &mode_config_funcs;


Object-level core funcs:
- struct drm_plane_funcs
- struct drm_crtc_funcs
- struct drm_connector_funcs
- struct drm_encoder_funcs
- struct drm_bridge_funcs

Examples:
- update_plane / disable_plane
- enable / disable
- destroy
- reset
- page flip handling

Rule:
- Core funcs are REQUIRED
- DRM core may call them anytime


HELPER FUNCS (OPTIONAL)
----------------------
Helper funcs are NOT called by DRM core.
They are called by DRM helper code
(e.g. drm_atomic_helper_*).

They define HOW the hardware should be driven.

Global helper funcs:
- struct drm_mode_config_helper_funcs
  - atomic_check
  - atomic_commit

Registered as:
  dev->mode_config.helper_private =
        &mode_config_helper_funcs;


Object-level helper funcs:
- struct drm_plane_helper_funcs
- struct drm_crtc_helper_funcs
- struct drm_connector_helper_funcs
- struct drm_encoder_helper_funcs

Examples:
- atomic_check
- atomic_update
- atomic_enable / atomic_disable
- mode_valid


KEY DIFFERENCES
---------------
Core funcs:
- Mandatory
- Called by DRM core
- Define hardware capability
- Entry points from userspace ioctls

Helper funcs:
- Optional
- Called by DRM helpers
- Provide sequencing and defaults
- Reduce driver complexity


TYPICAL MODERN ATOMIC DRIVER
----------------------------
1) Register core funcs:
   dev->mode_config.funcs = &mode_config_funcs;

2) Register helper funcs:
   dev->mode_config.helper_private =
        &mode_config_helper_funcs;

3) Register object funcs:
   drm_universal_plane_init(..., &plane_funcs, ...);
   drm_plane_helper_add(plane, &plane_helper_funcs);


EXECUTION FLOW (ATOMIC)
----------------------
mode_config_funcs.atomic_commit
  -> drm_atomic_helper_commit()
     -> plane_helper.atomic_update()
     -> crtc_helper.atomic_enable()
     -> vblank wait
     -> cleanup old state


WHY HELPERS ARE IMPORTANT
-------------------------
Without helpers, driver must:
- walk atomic state manually
- order plane/CRTC enable & disable
- manage vblank waits
- handle async commits
- manage FB lifetime

This is complex and error-prone.

Helpers encode years of DRM bug fixes.


TL;DR
-----
Core funcs   = required DRM core contract
Helper funcs = optional shared implementation layer

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
atomic_commit_tail() and drm_atomic_helper_commit_tail() (COPY-PASTE NOTES)
===========================================================================

WHERE THESE FIT
--------------
In an atomic KMS driver, you typically implement:

  dev->mode_config.funcs->atomic_commit
        |
        v
  drm_atomic_helper_commit()
        |
        v
  drm_atomic_helper_commit_tail()   <-- "tail" = the real HW programming phase

Think of atomic commit as two phases:
  (A) PREP / book-keeping
  (B) TAIL / hardware programming + event arming + cleanup

The "tail" is phase (B).


WHAT IS "atomic_commit_tail"?
-----------------------------
atomic_commit_tail is not a standalone DRM core callback like
mode_config.funcs->atomic_commit.

Instead, it is a helper-level concept:
- The atomic helpers split commit work so that "tail" can run
  in a safe context (often in a worker thread) and can SLEEP.

Drivers sometimes provide their own "commit_tail" function
(or override pieces of the default tail sequence) when they need
SoC-specific ordering.


WHAT drm_atomic_helper_commit_tail() DOES
-----------------------------------------
drm_atomic_helper_commit_tail() is the default helper routine that performs
the "hardware commit tail".

High level responsibilities:

1) Disable/enable sequencing (modeset part)
   - Disable CRTCs/encoders/bridges that are going off
   - Program modeset for CRTCs that change mode
   - Enable encoders/bridges/CRTCs in correct order

2) Plane programming (scanout part)
   - Call each plane's helper callbacks:
       plane_helper->atomic_update()
     or plane->atomic_update path via helpers depending on kernel
   - This is where FB addresses/strides/registers usually get programmed.

3) VBlank + event arming
   - Arm DRM pageflip/vblank events for CRTCs that requested them
   - Ensure vblank is enabled when needed

4) Fence / sync handling (as applicable)
   - Wait for input fences before programming (depends on helper path)
   - Signal output fences/events after programming

5) Cleanup old state
   - Release framebuffer references from the old state
   - Drop GEM references, unpin buffers (depending on driver)
   - drm_atomic_helper_cleanup_planes() and friends (conceptually)


WHY THE "TAIL" EXISTS
---------------------
Because parts of commit:
- must be sleepable (may wait for fences, runtime PM, I2C/DSI, etc.)
- must run outside locks taken during atomic_check / state swap
- may need to be queued for nonblock commits

So helpers do:
- fast state swap under locks
- schedule tail work in a worker for nonblock commits


drm_atomic_helper_commit() VS drm_atomic_helper_commit_tail()
-------------------------------------------------------------
drm_atomic_helper_commit(dev, state, nonblock):
- High-level wrapper
- Performs:
  * prepare (pin buffers, get runtime PM, etc. depending on kernel)
  * swap state into dev
  * queue tail (if nonblock) or run tail directly (if blocking)

drm_atomic_helper_commit_tail(state):
- The actual "program hardware + enable/disable + events + cleanup"
- Usually runs in process context and can sleep


WHEN YOU OVERRIDE / CUSTOMIZE THE TAIL
--------------------------------------
Use the default drm_atomic_helper_commit_tail() if:
- normal enable/disable ordering works
- your plane/crtc/connector helper callbacks do the HW programming correctly

Override / wrap the tail when:
- your SoC needs strict ordering across multiple CRTCs/pipelines
- shared PLL/clock programming must happen at a specific point
- bridges/panels require special delays or command sequences
- you have special runtime PM or bandwidth programming to do
  before/after planes are updated

Common pattern:
- call drm_atomic_helper_commit_tail() but add your steps around it.


COMMON DRIVER PATTERN (PSEUDO)
------------------------------
mode_config_funcs.atomic_commit:
  return drm_atomic_helper_commit(dev, state, nonblock);

You customize by replacing atomic_commit and running your own tail:

atomic_commit(dev, state, nonblock):
  ...
  if (nonblock)
     queue_work(..., my_commit_tail_work);
  else
     my_commit_tail(state);

my_commit_tail(state):
  // SoC pre-steps (clocks/bw)
  drm_atomic_helper_commit_tail(state);
  // SoC post-steps


IMPORTANT RULES / PITFALLS
--------------------------
- Do NOT program HW in atomic_check().
- Tail is where HW register programming normally happens.
- Tail may sleep -> do not run it in atomic context.
- In nonblock commits, tail must run in a worker thread.
- Be careful with vblank enabling/disabling and event arming ordering.

TL;DR
-----
- "commit_tail" = commit phase that actually touches HW and can sleep.
- drm_atomic_helper_commit() is the wrapper.
- drm_atomic_helper_commit_tail() is the default implementation of the HW tail.

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
DRM ATOMIC COMMIT NOTES
======================

BLOCKING vs NON-BLOCKING ATOMIC COMMIT
-------------------------------------

WHAT "BLOCK" AND "NONBLOCK" MEAN
-------------------------------
Both blocking and non-blocking atomic commits are fully atomic in terms of
state update.

The difference is NOT atomicity.
The difference is:
- where the commit tail runs
- whether the userspace ioctl blocks or returns immediately


BLOCKING ATOMIC COMMIT (nonblock = false)
-----------------------------------------
Definition:
- Atomic commit runs synchronously.
- The ioctl does NOT return until the commit is fully completed.

Execution flow:
userspace ioctl
  -> drm_atomic_commit()
     -> mode_config.funcs->atomic_commit(dev, state, false)
        -> drm_atomic_helper_commit(dev, state, false)
           -> drm_atomic_helper_commit_tail(state)
              -> program hardware
              -> wait for vblank(s) if required
              -> arm events
              -> cleanup old state
  <- ioctl returns AFTER everything finishes

Key points:
- commit_tail runs in the same context as the ioctl
- if commit_tail waits for vblank, userspace waits too
- userspace thread is blocked the entire time

Effects:
- userspace cannot render next frame
- userspace cannot submit GPU work
- userspace cannot process events/input (single-threaded)

Typical usage:
- initial modeset
- bring-up and debugging
- test tools like modetest

Pros:
- simple and deterministic
- easy to debug
- success return means commit fully finished

Cons:
- poor performance for continuous rendering
- no pipelining
- can cause stutter if used per frame


NON-BLOCKING ATOMIC COMMIT (nonblock = true)
--------------------------------------------
Definition:
- Atomic commit is asynchronous.
- The ioctl returns immediately.

Execution flow:
userspace ioctl
  -> drm_atomic_commit()
     -> mode_config.funcs->atomic_commit(dev, state, true)
        -> drm_atomic_helper_commit(dev, state, true)
           -> swap atomic state
           -> queue commit_tail work
  <- ioctl returns IMMEDIATELY

Later (worker thread):
  -> drm_atomic_helper_commit_tail(state)
     -> program hardware
     -> drm_atomic_helper_wait_for_vblanks(...)
     -> arm page-flip events
     -> cleanup old state

Key points:
- commit_tail runs in a worker thread
- any vblank wait blocks only the worker, not userspace
- userspace continues execution immediately

Effects:
- userspace can render the next frame
- GPU and CPU work can overlap with scanout
- event-driven completion

Typical usage:
- page flips
- compositors (Wayland/X11)
- smooth animations and video playback

Pros:
- enables pipelining
- better frame pacing
- required for modern compositors

Cons:
- more complex
- requires correct event handling
- harder to debug


WHY USERSACE STILL "WAITS"
-------------------------
In non-blocking commits:
- userspace does NOT wait in the ioctl
- userspace waits via poll/select on the DRM fd
- completion is signaled via events

Userspace typically waits for:
- DRM_MODE_PAGE_FLIP_EVENT (most common)

Page-flip event:
- delivered at vblank
- indicates the new framebuffer is live
- stronger guarantee than a raw vblank event


WHY commit_tail() WAITS FOR VBLANK
---------------------------------
drm_atomic_helper_commit_tail() may call:
  drm_atomic_helper_wait_for_vblanks()

This wait is required for:
- correct page-flip event timing
- safe cleanup of old framebuffers
- proper modeset sequencing

Important:
- NONBLOCK means "do not block the ioctl"
- it does NOT mean "kernel must not wait"

In blocking commits:
- userspace waits for vblank

In non-blocking commits:
- only the worker thread waits
- userspace continues running


MENTAL MODEL
------------
Blocking commit:
- "Do everything now, wait until finished"

Non-blocking commit:
- "Queue the work, notify me when it is visible"


SUMMARY
-------
- Blocking commit stalls userspace until completion
- Non-blocking commit keeps userspace running
- commit_tail may wait for vblank in both cases
- NONBLOCK moves the wait out of the ioctl path
- Page-flip events provide completion notification

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
DRM CORE FUNCS vs HELPER FUNCS (NOTES)
======================================

BIG IDEA
--------
DRM/KMS has 2 layers of callbacks:

1) CORE FUNCS    (mandatory contract with DRM core)
2) HELPER FUNCS  (optional callbacks used by DRM helper code)

Core funcs answer: "What operations exist and how does core manage objects?"
Helper funcs answer: "How do we actually program HW for atomic modeset?"

If core funcs are missing -> driver will not work.
If helper funcs are missing -> driver can still work, but you must implement
much more logic yourself (sequencing, commit flow, etc).


WHO CALLS WHAT
--------------
CORE FUNCS:
- called directly by DRM core (ioctl paths, object lifetime, vblank core)

HELPER FUNCS:
- called by DRM helper libraries
  (drm_atomic_helper_*, drm_plane_helper_*, drm_crtc_helper_*)


WHERE TO PUT HARDWARE PROGRAMMING
--------------------------------
- atomic_check(): validate only, no register writes
- hardware register programming typically happens in:
  * plane_helper.atomic_update()
  * crtc_helper.atomic_enable()/atomic_disable()/atomic_flush()
  * or driver-specific commit_tail around drm_atomic_helper_commit_tail()

Core funcs are not the place for most register programming. They are the
framework integration points and object/state management.


drm_plane_funcs vs drm_plane_helper_funcs
=========================================

drm_plane_funcs (CORE, mandatory for a plane)
---------------------------------------------
Purpose:
- object lifetime and DRM core interface
Typical members:
- update_plane / disable_plane (legacy path)
- destroy
- reset
- atomic_duplicate_state
- atomic_destroy_state
- atomic_set_property / atomic_get_property (or property helpers)

Called by:
- DRM core (legacy ioctls and state management)

What you do here:
- implement state allocation/copy/free (atomic_*_state)
- provide legacy hooks if you support legacy
- integrate with DRM core object model

Rule of thumb:
- Treat as "API contract + state/lifecycle"


drm_plane_helper_funcs (HELPER, optional but recommended)
---------------------------------------------------------
Purpose:
- atomic helper programming for the plane
Typical members:
- atomic_check   : validate plane state
- atomic_update  : program plane registers (fb addr, stride, format, src/dst)
- atomic_disable : disable plane in HW

Called by:
- drm_atomic_helper_commit_tail() / atomic helpers, not the DRM core directly

What you do here:
- real plane HW programming (most common place)
- format/stride/scaling checks (in atomic_check)


Atomic flow (plane relevant, simplified):
-----------------------------------------
drmModeAtomicCommit()
  -> drm_atomic_helper_commit()
     -> plane_helper.atomic_check()
     -> ...
     -> drm_atomic_helper_commit_tail()
        -> plane_helper.atomic_update() / atomic_disable()


TL;DR (plane)
-------------
drm_plane_funcs        = plane object + state/lifetime + legacy integration
drm_plane_helper_funcs = atomic helper plane programming (HW update/disable)


drm_crtc_funcs vs drm_crtc_helper_funcs
=======================================

drm_crtc_funcs (CORE, mandatory for a CRTC)
-------------------------------------------
Purpose:
- core contract for CRTC object and vblank integration
Typical members:
- destroy
- reset
- set_config (legacy)
- page_flip (legacy)
- atomic_duplicate_state
- atomic_destroy_state
- enable_vblank / disable_vblank
- get_vblank_counter (optional)

Called by:
- DRM core

What you do here:
- manage CRTC state objects (duplicate/destroy/reset)
- implement vblank enable/disable hooks for your hardware interrupt logic
- legacy set_config/page_flip only if needed


drm_crtc_helper_funcs (HELPER, optional but recommended)
--------------------------------------------------------
Purpose:
- atomic helper programming for CRTC pipeline and modeset sequencing
Typical members:
- atomic_check
- atomic_begin     (optional)
- atomic_enable    (turn on pipeline, clocks, timing generator)
- atomic_disable   (turn off pipeline)
- atomic_flush     (kick/commit updates, arm vblank/pageflip event)
- mode_valid       (validate mode against HW limits)

Called by:
- atomic helpers (commit_tail sequencing)

What you do here:
- program CRTC mode/timings
- enable/disable display pipeline blocks
- handle flush/event arming ordering (often where pageflip completes)


Atomic flow (CRTC relevant, simplified):
----------------------------------------
drmModeAtomicCommit()
  -> drm_atomic_helper_commit()
     -> crtc_helper.atomic_check()
     -> ...
     -> drm_atomic_helper_commit_tail()
        -> crtc_helper.atomic_disable() (old)
        -> crtc_helper.atomic_enable()  (new)
        -> plane_helper.atomic_update()
        -> crtc_helper.atomic_flush()


TL;DR (crtc)
------------
drm_crtc_funcs        = CRTC object + state/lifetime + vblank core hooks
drm_crtc_helper_funcs = atomic helper modeset + enable/disable + flush sequence


ONE-LINE MEMORY
---------------
Core funcs   = "DRM core interface + state/lifecycle"
Helper funcs = "Atomic helper sequencing + real HW programming"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Drivers must define the file operations structure that forms the DRM         
 * userspace API entry point, even though most of those operations are          
 * implemented in the DRM core. The resulting &struct file_operations must be   
 * stored in the &drm_driver.fops field. The mandatory functions are drm_open(),
 * drm_read(), drm_ioctl() and drm_compat_ioctl()

drm_driver.open()
*
* Driver callback when a new &struct drm_file is opened. Useful for
* setting up driver-private data structures like buffer allocators,
	* execution contexts or similar things.
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
* The driver must protect regions that is accessing device resources to prevent use after they’re released. This is done using drm_dev_enter() and drm_dev_exit()
Todo
drm_dev_enter(), drm_dev_exit()?
blocking and non blocking commits, updates
