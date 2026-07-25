include ../../Makefile.include JAVA-JRE-VERSION ../../download-files.include
DEPS = ../../Makefile.include Makefile JAVA-JRE-VERSION ../../download-files.include \
                      01-8245112-fix-jcheck-conf-for-Mobile-project.patch \
                      02-8244947-fix-ios-android-build-for-static-libs.patch \
                      03-8333357-revert-current-mobile-changes.patch \
                      04-8333795-mobile-allow-building-jdk-native-libs-for-io.patch \
                      05-8340467-mobile-duplicate-symbols-in-libprefs.a-on-iO.patch \
                      06-8346233-mobile-enable-static-libs-image-for-iOS.patch \
                      07-8347033-mobile-add-ios-specific-build-docs.patch \
                      08-8357119-Make-Zero-the-default-variant-of-HotSpot-for.patch \
                      09-8359163-Fix-path-to-current-module-on-ios.patch \
                      10-8370659-mobile-enable-static-libs-image-for-Android.patch \
                      11-8374535-IOS-uses-different-path-for-modules.patch \
                      12-8374522-Mobile-Add-iOS-as-a-platform-in-OperatingSys.patch \
                      13-8380088-IOS-jit_write_protect_np-not-allowed.patch \
                      14-8365887-Outdated-comments-in-String-decode.patch




CONFIGURE += ./configure --openjdk-target=aarch64-android-linux
                         --with-freetype-include=$(PREFIX)/include/freetype2 \
                         --with-freetype-lib=$(PREFIX)/lib \
                         --with-cups=$(PREFIX)/share/cups/cups \
                         --disable-precompiled-headers \
                         --disable-warnings-as-errors \
                         --enable-option-checking=fatal \
                         --enable-headless-only=yes

ifeq ($(OS),android)
  CONFIGURE+= --with-libffi=$(PREFIX) \
              --with-fontconfig=$(PREFIX) \
              --with-fontconfig-include=$(PREFIX)/include \
              --with-toolchain-type=clang
endif

# ios     configure \
#         --disable-warnings-as-errors \
#         --openjdk-target=aarch64-macos-ios \
#         --with-libffi-include=<support-dir-path>/libffi/include \
#         --with-libffi-lib=<support-dir-path>/libffi/libs \
#         --with-cups-include=<support-dir-path>/cups-2.3.6
#         --with-sysroot=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk \


# Build has following in tree, but we might be better to point to ours as we build all of these
#--with-libjpeg=<source> - Use the specified source for libjpeg
#--with-giflib=<source> - Use the specified source for giflib
#--with-libpng=<source> - Use the specified source for libpng
#--with-lcms=<source> - Use the specified source for lcms
#--with-zlib=<source> - Use the specified source for zlib


#bash ./configure \
#    --openjdk-target=$TARGET \
#    --with-extra-cflags="$CFLAGS" \
#    --with-extra-cxxflags="$CFLAGS" \
#    --with-extra-ldflags="$LDFLAGS" \
#    --disable-precompiled-headers \
#    --disable-warnings-as-errors \
#    --enable-option-checking=fatal \
#    --enable-headless-only=yes \
#    --with-jvm-variants=$JVM_VARIANTS \
#    --with-jvm-features=-dtrace,-zero,-vm-structs,-epsilongc \
#    --with-cups-include=$CUPS_DIR \
#    --with-devkit=$TOOLCHAIN \
#    --with-native-debug-symbols=external \
#    --with-debug-level=$JDK_DEBUG_LEVEL \
#    --with-fontconfig-include=$ANDROID_INCLUDE \

#  if [[ $TARGET_VERSION -eq 21 ]]; then
#    platform_args+="--build=x86_64-unknown-linux-gnu \
#    "
#  fi

#  platform_args+="OBJCOPY=${OBJCOPY} \

export AR RANLIB STRIP

export CUPS_DIR=$(PREFIX)/share/cups
export CFLAGS+=" -DLE_STANDALONE"

export CFLAGS+=" -mllvm -polly -DANDROID -Wno-error=implicit-function-declaration -Wno-error=int-conversion"
export LDFLAGS+=" -L$(PLATFORM)/dummy_libs -Wl,--undefined-version"


LIBDYLIB=$(PLATFORM)/build/lib/$(BYPRODUCT)

all: .installed-$(PLATFORM)

.configured-$(PLATFORM): $(DEPS) | $(TARBALLS_LOCATION)/$(ARCHIVE).$(HASH_TYPE)
	rm -rf $(PLATFORM)/*; mkdir -p $(PLATFORM)
	cd $(PLATFORM); $(ARCHIVE_TOOL) $(ARCHIVE_TOOL_FLAGS) $(TARBALLS_LOCATION)/$(ARCHIVE)
	cd $(PLATFORM); patch -p1 -i ../01-8245112-fix-jcheck-conf-for-Mobile-project.patch
	cd $(PLATFORM); patch -p1 -i ../02-8244947-fix-ios-android-build-for-static-libs.patch
	cd $(PLATFORM); patch -p1 -i ../03-8333357-revert-current-mobile-changes.patch
	cd $(PLATFORM); patch -p1 -i ../04-8333795-mobile-allow-building-jdk-native-libs-for-io.patch
	cd $(PLATFORM); patch -p1 -i ../05-8340467-mobile-duplicate-symbols-in-libprefs.a-on-iO.patch
	cd $(PLATFORM); patch -p1 -i ../06-8346233-mobile-enable-static-libs-image-for-iOS.patch
	cd $(PLATFORM); patch -p1 -i ../07-8347033-mobile-add-ios-specific-build-docs.patch
	cd $(PLATFORM); patch -p1 -i ../08-8357119-Make-Zero-the-default-variant-of-HotSpot-for.patch
	cd $(PLATFORM); patch -p1 -i ../09-8359163-Fix-path-to-current-module-on-ios.patch
	cd $(PLATFORM); patch -p1 -i ../10-8370659-mobile-enable-static-libs-image-for-Android.patch
	cd $(PLATFORM); patch -p1 -i ../11-8374535-IOS-uses-different-path-for-modules.patch
	cd $(PLATFORM); patch -p1 -i ../12-8374522-Mobile-Add-iOS-as-a-platform-in-OperatingSys.patch
	cd $(PLATFORM); patch -p1 -i ../13-8380088-IOS-jit_write_protect_np-not-allowed.patch
	cd $(PLATFORM); patch -p1 -i ../14-8365887-Outdated-comments-in-String-decode.patch
# Create dummy libraries so we won't have to remove them in OpenJDK makefiles
	cd $(PLATFORM); mkdir -p dummy_libs
	cd $(PLATFORM); ar cru dummy_libs/libpthread.a
	cd $(PLATFORM); ar cru dummy_libs/librt.a
	cd $(PLATFORM); ar cru dummy_libs/libthread_db.a
	cd $(PLATFORM); $(CONFIGURE)
	touch $@

$(LIBDYLIB): .configured-$(PLATFORM)
	$(MAKE) -C $(PLATFORM)/build

.installed-$(PLATFORM): $(LIBDYLIB)
	$(MAKE) -C $(PLATFORM)/build install
	touch $@

clean:
	$(MAKE) -C $(PLATFORM) clean
	rm -f .installed-$(PLATFORM) .configured-$(PLATFORM)

distclean::
	rm -rf $(PLATFORM) .installed-$(PLATFORM) .configured-$(PLATFORM)
