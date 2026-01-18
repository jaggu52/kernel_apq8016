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

