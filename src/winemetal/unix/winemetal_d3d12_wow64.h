/* D3D12 uses the same fixed-width Metal handles as D3D11. Only CPU pointers
 * are rebased; GPU addresses and Objective-C handles must remain unchanged. */
/* Madeira owns the shader request layout. args already names host memory;
 * the adapter rebases its nested guest pointers and restores them on return.
 * Older hosts may not export this adapter yet; retain the previous refusal
 * until the host is updated. */
extern int madeira_ir_convert_wow64_unix(void *, uintptr_t) __attribute__((weak_import));

static NTSTATUS _madeira_ir_convert_wow64(void *args)
{
  if (!madeira_ir_convert_wow64_unix) return STATUS_NOT_IMPLEMENTED;
  return madeira_ir_convert_wow64_unix(args, (uintptr_t)UInt32ToPtr(1) - 1);
}

static NTSTATUS _MTLDevice_newRenderPipelineStateVD_wow64(void *args)
{
  struct unixcall_mtldevice_newrenderpso_vd *guest = args, native = *guest;
  const struct WMTRenderPipelineInfo *input = wow_in(guest->info.ptr);
  struct WMTRenderPipelineInfo info;
  NTSTATUS status;
  guest->ret_pso = guest->ret_error = 0;
  if (!input || !guest->vd.ptr) return STATUS_INVALID_PARAMETER;
  info = *input;
  info.binary_archives_for_lookup.ptr = wow_in(info.binary_archives_for_lookup.ptr);
  native.info.ptr = &info;
  native.vd.ptr = wow_in(guest->vd.ptr);
  status = _MTLDevice_newRenderPipelineStateVD(&native);
  guest->ret_pso = native.ret_pso;
  guest->ret_error = native.ret_error;
  return status;
}

static NTSTATUS _MTLDevice_newGeometryEmulationPipelineState_wow64(void *args)
{
  struct unixcall_mtldevice_newgeompso *guest = args, native = *guest;
  const struct WMTMeshRenderPipelineInfo *input = wow_in(guest->info.ptr);
  struct WMTMeshRenderPipelineInfo info;
  NTSTATUS status;
  guest->ret_pso = guest->ret_error = 0;
  if (!input || !guest->ge.ptr) return STATUS_INVALID_PARAMETER;
  info = *input;
  info.binary_archives_for_lookup.ptr = wow_in(info.binary_archives_for_lookup.ptr);
  native.info.ptr = &info;
  native.ge.ptr = wow_in(guest->ge.ptr);
  status = _MTLDevice_newGeometryEmulationPipelineState(&native);
  guest->ret_pso = native.ret_pso;
  guest->ret_error = native.ret_error;
  return status;
}

static NTSTATUS _MTLDevice_heapTextureSizeAndAlign_wow64(void *args)
{
  struct unixcall_mtldevice_heaptexturesizealign *guest = args, native = *guest;
  NTSTATUS status;
  guest->ret_size = guest->ret_align = 0;
  native.info.ptr = wow_in(guest->info.ptr);
  if (!native.info.ptr) return STATUS_INVALID_PARAMETER;
  status = _MTLDevice_heapTextureSizeAndAlign(&native);
  guest->ret_size = native.ret_size;
  guest->ret_align = native.ret_align;
  return status;
}

static NTSTATUS _MTLHeap_newTextureAtOffset_wow64(void *args)
{
  struct unixcall_mtlheap_newtextureatoffset *guest = args, native = *guest;
  NTSTATUS status;
  guest->ret = 0;
  native.info.ptr = wow_in(guest->info.ptr);
  if (!native.info.ptr) return STATUS_INVALID_PARAMETER;
  status = _MTLHeap_newTextureAtOffset(&native);
  guest->ret = native.ret;
  return status;
}

static NTSTATUS _MTLHeap_newBufferAtOffset_wow64(void *args)
{
  struct unixcall_mtlheap_newbufferatoffset *guest = args, native = *guest;
  struct WMTBufferInfo *info = wow_in(guest->info.ptr);
  NTSTATUS status;
  guest->ret = 0;
  if (!info) return STATUS_INVALID_PARAMETER;
  /* A private placed buffer has no CPU mapping. A shared heap needs a guest
   * mapping for the entire heap, which this interface does not provide. */
  if ((info->options & 0x30) != WMTResourceStorageModePrivate)
    return STATUS_INVALID_ADDRESS;
  native.info.ptr = info;
  status = _MTLHeap_newBufferAtOffset(&native);
  guest->ret = native.ret;
  return status;
}
