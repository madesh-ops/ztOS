# Bluetooth on the Maxwell subsystem: how Samsung's driver does it

From Samsung's M20 kernel (crDroid `android_kernel_samsung_exynos7885`,
branch 15.0): `drivers/misc/samsung/scsc_bt/` and `include/scsc/api/bhcs.h`,
`bsmhcp.h`. Bluetooth runs on the subsystem's M4, on top of the same boot and
management transport `drivers/zrn-wlbt.c` sets up. Nothing here is built yet.

## Prerequisite

The firmware must have answered MM_START_IND (`zrn-wlbt.c`, `boot`).
Bluetooth is then a *service* started over the management transport.

## Starting the service

1. Open service `SCSC_SERVICE_ID_BT` (= 2).
2. Allocate in the window (32-byte aligned, `BSMHCP_ALIGNMENT`):
   - the `BSMHCP_PROTOCOL` block: the transfer rings below plus their indices;
   - optionally a configuration blob (`bt.hcf`, looked up under the firmware
     directory's `bluetooth/` configuration path, as `mx140_file.c` does for
     `wlan.hcf`; missing is fine);
   - the `struct BHCS` start block:
     `version = 2`, `bsmhcp_protocol_offset/length`, `configuration_offset/length`,
     and the Bluetooth address as LAP (24 bit), UAP (8), NAP (16), which
     Samsung reads from `/csa/bluetooth/.bd_addr` (the CSA partition).
3. Send `SM_MSG_START_REQ` on management channel 1 (service management):
   `struct sm_msg_packet { u8 service_id; u8 msg; u32 optional_data; }`
   packed, `optional_data` = the BHCS offset. Wait for `SM_MSG_START_CFM`.
4. Stop: `SM_MSG_STOP_REQ`, wait for `SM_MSG_STOP_CFM`.

## The HCI transport (BSMHCP)

Rings in the window, each with a read/write index pair (`struct BSMHCP_INDEX`):

| Ring | Entries | Entry | Direction |
|---|---|---|---|
| HCI commands | 8 | `BSMHCP_TD_CONTROL` (length + 258 bytes) | host → controller |
| HCI events | 32 | `BSMHCP_TD_HCI_EVT` (length, handle, event type, 258 bytes) | controller → host |
| ACL TX | 32 data + 32 free | `BSMHCP_TD_ACL_TX_DATA` pointing at one of 36 1021-byte buffers | host → controller |
| ACL RX | 32 | `BSMHCP_TD_ACL_RX` (handle, length, flags, 1024 bytes) | controller → host |
| AVDTP, IQ reports, timing | 16 / 8 / 64 | detection helpers | — |

Each ring direction is signalled with a mailbox interrupt bit to the M4
(from-AP) or to the AP; the bits are allocated like the management ones and
named in the protocol block.

## The mainline shape

Samsung's `scsc_bluez.c` already bridges these rings to a BlueZ `hci_dev`.
A mainline driver would be that bridge alone: an `hci_dev` whose `send()`
writes the command/ACL rings and whose interrupt handler feeds events and
ACL RX to `hci_recv_frame()`. The H4 character device, AVDTP detection, ANT
and the logging rings can stay out.

The M20 has no BT-ABOX window (`WIFI2AP_MEM_CONFIG2/3` stay 0, as in
Samsung's tree), so Bluetooth audio offload is not part of this.
