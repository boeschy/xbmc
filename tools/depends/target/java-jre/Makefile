include ../../Makefile.include JAVA-JRE-VERSION ../../download-files.include
DEPS = ../../Makefile.include Makefile JAVA-JRE-VERSION ../../download-files.include \
                      01-8245112-fix-jcheck-conf-for-Mobile-project.patch \
                      02-8244947-fix-ios-android-build-for-static-libs.patch \
                      03-8333357-revert-current-mobile-changes.patch \
                      04-8333795-mobile-allow-building-jdk-native-libs-for-io.patch \
                      05-8340467-mobile-duplicate-symbols-in-libprefs.a-on-iO.patch \
                      07-8347033-mobile-add-ios-specific-build-docs.patch \
                      08-8357119-Make-Zero-the-default-variant-of-HotSpot-for.patch \
                      10-8370659-mobile-enable-static-libs-image-for-Android.patch \
                      12-8374522-Mobile-Add-iOS-as-a-platform-in-OperatingSys.patch \
                      13-fix-android.patch \




ifeq ($(OS),android)
  CONFIGURE+= ./configure --build=x86_64-unknown-linux-gnu \
              --openjdk-target=aarch64-linux-android \
              --with-toolchain-type=clang \
              --with-toolchain-path=$(TOOLCHAIN)/bin \
              --with-sysroot=$(TOOLCHAIN)/sysroot \
              BUILD_CC=$$(which clang) \
              BUILD_CXX=$$(which clang++) \
              CC=$(TOOLCHAIN)/bin/aarch64-linux-android24-clang \
              CXX=$(TOOLCHAIN)/bin/aarch64-linux-android24-clang++ \
              LD=$(TOOLCHAIN)/bin/ld.lld \
              AR=$(TOOLCHAIN)/bin/llvm-ar \
              NM=$(TOOLCHAIN)/bin/llvm-nm \
              OBJCOPY=$(TOOLCHAIN)/bin/llvm-objcopy \
              OBJDUMP=$(TOOLCHAIN)/bin/llvm-objdump \
              STRIP=$(TOOLCHAIN)/bin/llvm-strip \
              --disable-precompiled-headers \
              --disable-warnings-as-errors \
              --enable-option-checking=fatal \
              --enable-headless-only=yes \
              --with-cups-include=$(PREFIX)/share/cups \
              --with-extra-cflags=" -DLE_STANDALONE -mllvm -polly -DANDROID -Wno-error=implicit-function-declaration -Wno-error=int-conversion" \
              --with-extra-ldflags=" -L$(PLATFORM)/dummy_libs  -Wl,--undefined-version -Wl,--allow-shlib-undefined" \
              --with-fontconfig=$(PREFIX) \
              --with-fontconfig-include=$(PREFIX)/include
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
	cd $(PLATFORM); patch -p1 -i ../07-8347033-mobile-add-ios-specific-build-docs.patch
	cd $(PLATFORM); patch -p1 -i ../08-8357119-Make-Zero-the-default-variant-of-HotSpot-for.patch
	cd $(PLATFORM); patch -p1 -i ../10-8370659-mobile-enable-static-libs-image-for-Android.patch
	cd $(PLATFORM); patch -p1 -i ../12-8374522-Mobile-Add-iOS-as-a-platform-in-OperatingSys.patch
	cd $(PLATFORM); patch -p1 -i ../13-fix-android.patch
# Create dummy libraries so we won't have to remove them in OpenJDK makefiles
	cd $(PLATFORM); mkdir -p dummy_libs
	cd $(PLATFORM); ar cru dummy_libs/libpthread.a
	cd $(PLATFORM); ar cru dummy_libs/librt.a
	cd $(PLATFORM); ar cru dummy_libs/libthread_db.a
	cd $(PLATFORM); bash $(CONFIGURE)
	touch $@

$(LIBDYLIB): .configured-$(PLATFORM)
	$(MAKE) -j1 -C $(PLATFORM) images


.installed-$(PLATFORM): $(LIBDYLIB)
	touch $@

clean:
	$(MAKE) -C $(PLATFORM) clean
	rm -f .installed-$(PLATFORM) .configured-$(PLATFORM)

distclean::
	rm -rf $(PLATFORM) .installed-$(PLATFORM) .configured-$(PLATFORM)
