/** @file
  SerialIoProbe — throwaway EFI app that writes a unique magic string via
  gEfiSerialIoProtocol to verify the string lands in \UefiLog<N>.txt on
  canoe.  Remove after Task 0 gate passes (logging-v2 plan 2026-05-13).
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Protocol/SerialIo.h>

#define MAGIC "GBL_SERIALIO_PROBE_MAGIC_2026_05_13_4F8A2B7C\n"

EFI_STATUS
EFIAPI
SerialIoProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_SERIAL_IO_PROTOCOL *SerialIo;
  UINTN                 NumHandles;
  EFI_HANDLE            *Handles;
  UINTN                 Written;
  UINTN                 i;

  Print (L"SerialIoProbe: start\n");

  Status = SystemTable->BootServices->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSerialIoProtocolGuid,
                  NULL,
                  &NumHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    Print (L"SerialIoProbe: LocateHandleBuffer(SerialIo) => %r  (no SerialIo handles)\n", Status);
    return Status;
  }

  Print (L"SerialIoProbe: found %u SerialIo handle(s)\n", (UINT32)NumHandles);

  for (i = 0; i < NumHandles; i++) {
    Status = SystemTable->BootServices->HandleProtocol (
                    Handles[i],
                    &gEfiSerialIoProtocolGuid,
                    (VOID **)&SerialIo
                    );
    if (EFI_ERROR (Status)) {
      Print (L"SerialIoProbe: handle %u HandleProtocol => %r\n", (UINT32)i, Status);
      continue;
    }

    Written = sizeof (MAGIC) - 1;  /* exclude NUL */
    Status  = SerialIo->Write (SerialIo, &Written, (VOID *)MAGIC);
    Print (L"SerialIoProbe: handle %u Write => %r  bytes=%u\n",
           (UINT32)i, Status, (UINT32)Written);
  }

  SystemTable->BootServices->FreePool (Handles);

  /* Also emit via DEBUG() so GblDebugLib's sink sees it if SerialIo is absent */
  DEBUG ((DEBUG_ERROR, "SerialIoProbe: " MAGIC));

  Print (L"SerialIoProbe: done\n");
  return EFI_SUCCESS;
}
