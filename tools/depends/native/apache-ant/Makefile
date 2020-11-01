include ../../Makefile.include
PREFIX=$(NATIVEPREFIX)
PLATFORM=$(NATIVEPLATFORM)

DEPS= ../../Makefile.include Makefile 01-fix-chmod.patch

# lib name, version
LIBNAME=apache-ant
VERSION=1.10.6
SOURCE=$(LIBNAME)-$(VERSION)-src
ARCHIVE=$(SOURCE).tar.gz
BASE_URL=https://archive.apache.org/dist/ant/source

LIBDYLIB=$(PLATFORM)/dist/bin/ant

all: .installed-$(PLATFORM)

$(TARBALLS_LOCATION)/$(ARCHIVE):
	cd $(TARBALLS_LOCATION); $(RETRIEVE_TOOL) $(RETRIEVE_TOOL_FLAGS) $(BASE_URL)/$(ARCHIVE)

$(PLATFORM): $(TARBALLS_LOCATION)/$(ARCHIVE) $(DEPS)
	rm -rf $(PLATFORM)/*; mkdir -p $(PLATFORM)
	cd $(PLATFORM); $(ARCHIVE_TOOL) $(ARCHIVE_TOOL_FLAGS) $(TARBALLS_LOCATION)/$(ARCHIVE)
	cd $(PLATFORM); patch -p1 -i ../01-fix-chmod.patch
	cd $(PLATFORM); ./bootstrap.sh

$(LIBDYLIB): $(PLATFORM)
	cd $(PLATFORM); ./build.sh

.installed-$(PLATFORM): $(LIBDYLIB)
	cd $(PLATFORM); ANT_HOME=$(PREFIX) ./build.sh install-lite
	touch $@

clean:
	cd $(PLATFORM); ./build.sh clean
	rm -f .installed-$(PLATFORM)

distclean::
	rm -rf $(PLATFORM) .installed-$(PLATFORM)
