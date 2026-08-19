include ../../Makefile.include EDGE264-MVC-VERSION ../../download-files.include
DEPS = ../../Makefile.include EDGE264-MVC-VERSION Makefile \
       01-android-portability.patch ../../download-files.include

# edge264-mvc ships its own hand-written Makefile (OS=/STATIC=/PREFIX=/
# install: are first-class there, including OS=android) - drive that
# directly instead of going through its CMakeLists.txt, which exists for
# other consumers and just wraps the same Makefile in an
# ExternalProject_Add(). Going straight to the Makefile avoids a second
# layer of cross-compilation bookkeeping on top of what this depends
# system already provides via CC/AR/CFLAGS below.
LIBDYLIB=$(PLATFORM)/$(BYPRODUCT)

MAKE_ARGS = OS=android \
            STATIC=yes \
            BUILD_TEST=no \
            PREFIX=$(PREFIX) \
            CC=$(CC) \
            AR=$(AR)

ifneq ($(CFLAGS),)
  MAKE_ARGS += CFLAGS="$(CFLAGS)"
endif

all: .installed-$(PLATFORM)

.configured-$(PLATFORM): $(DEPS) | $(TARBALLS_LOCATION)/$(ARCHIVE).$(HASH_TYPE)
	rm -rf $(PLATFORM); mkdir -p $(PLATFORM)
	cd $(PLATFORM); $(ARCHIVE_TOOL) $(ARCHIVE_TOOL_FLAGS) $(TARBALLS_LOCATION)/$(ARCHIVE)
	# Upstream edge264-mvc as of the pinned commit doesn't build cleanly
	# against Bionic (Android's libc): aligned_alloc()'s declaration is
	# hidden below __ANDROID_API__ >= 28 regardless of what's actually on
	# the device, and pthread_cancel() is unimplemented on Bionic outright
	# rather than just undeclared. See the patch itself for the two fixes.
	cd $(PLATFORM); patch -p1 -i ../01-android-portability.patch
	touch $@

$(LIBDYLIB): .configured-$(PLATFORM)
	$(MAKE) -C $(PLATFORM) $(MAKE_ARGS)

.installed-$(PLATFORM): $(LIBDYLIB)
	$(MAKE) -C $(PLATFORM) $(MAKE_ARGS) install
	touch $@

clean:
	$(MAKE) -C $(PLATFORM) $(MAKE_ARGS) clean
	rm -f .installed-$(PLATFORM) .configured-$(PLATFORM)

distclean::
	rm -rf $(PLATFORM) .installed-$(PLATFORM) .configured-$(PLATFORM)
