KERNELDIR := /lib/modules/`uname -r`/build

rpmrelease = devel

all::
	$(MAKE) -C $(KERNELDIR) M=`pwd` "$$@"

tmpspec = .tmp.litevm-kmod.spec

rpm:	all
	mkdir -p ../BUILD ../RPMS/$$(uname -m)
	sed 's/^Release:.*/Release: $(rpmrelease)/' litevm-kmod.spec > $(tmpspec)
	rpmbuild --define="kverrel $$(uname -r)" \
		 --define="objdir $$(pwd)" \
		 --define="_topdir $$(pwd)/.." \
		-bb $(tmpspec)
