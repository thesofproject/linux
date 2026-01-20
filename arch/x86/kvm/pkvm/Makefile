# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I $(srctree)/arch/x86/kvm

pkvm-hyp-y			:= early_alloc.o

pkvm-hyp-$(CONFIG_PKVM_INTEL)	+= vmx/host_vmentry.o vmx/host_vmx.o

pkvm-obj 			:= $(patsubst %.o,%.pkvm.o,$(pkvm-hyp-y))
obj-$(CONFIG_PKVM_X86)		+= pkvm.o

targets += $(pkvm-obj) pkvm.lds

# When compiling with the $(objtool-enabled) is true, a single build-in object
# will be processed by the objtool after all of these objects are linked as a
# vmlinux.o if the objtool processing is delayed (represented by the
# $(delay-objtool) is true), otherwise the processing will be done when
# compiling it.
#
# For the later case, the $(objtool-enabled) is private to the rule of
# $(obj)/%.o. As the pKVM's object file is compiled via using the rule of
# $(obj)/%.pkvm.o, the $(objtool-enabled) is empty which represents a false for
# the pKVM. So if the $(delay-objtool) is false, meaning the objtool processing
# should be done when compiling a single build-in object, but the pKVM's object
# will not be processed by the objtool as $(objtool-enabled) is false. Setting
# pKVM's $(objtool-enabled) variable according to the $(delay-objtool) so that
# the pKVM's objects will be processed by the objtool by either way.
ifdef CONFIG_OBJTOOL
$(obj)/%.pkvm.o: private objtool-enabled = $(if $(delay-objtool),,y)
endif

$(obj)/%.pkvm.o: $(src)/%.c FORCE
	$(call if_changed_rule,cc_o_c)
$(obj)/%.pkvm.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)

$(obj)/pkvm.lds: $(src)/pkvm.lds.S FORCE
	$(call if_changed_dep,cpp_lds_S)

LDFLAGS_pkvm.o := -r -T
$(obj)/pkvm.o: $(obj)/pkvm.lds $(addprefix $(obj)/,$(pkvm-obj)) FORCE
	$(call if_changed,ld)
