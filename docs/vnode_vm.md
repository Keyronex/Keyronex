Options:
1. The VM object and the VNode could both NOT take a reference on each other.
2. The VM object could take a refernece on the VNode but the VNode would NOT
   take a refernece on the VM object.
3. The VNode could take a reference on the VM object but the VM object would NOT
   take a reference on the vnode.

Variant of 3: The VM object could take a reference on the VNode while it has
dirty pages and release it when it hasn't.

If we go with 3 - we need to synchronise the vnode destruction with respect to
the VM object. The VM object lock probably has to be acquired to safely extract
the vnode.

Otherwise I see no reasons why not to go with 3.

~~~

To hell with all the above - mapping a vm_object which has a vnode now passes
through refcounting to the vnode itself.
