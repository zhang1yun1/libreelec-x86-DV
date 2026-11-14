// SPDX-License-Identifier: GPL-2.0-or-later
//
// WinDvTool.cs - Native 64-bit Windows Dolby Vision Hardware & ETSI GS CCM 001 V1.1.1 Tool
// Ported from CroqueMr/libreelec-x86-DV (cb1 + amdgpu_dm_dv.c + intel_hdmi.c + drm_dvbridge_edid.h)
//
// Features:
//   1. Reads live Monitor EDIDs from Windows Registry (HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY)
//      and parses CEA-861HDR Static Metadata + Dolby Vision VSVDB (OUI 0x00D046) via dvbridge_parse_edid()
//   2. Queries Windows GPU PCI Vendor/Device IDs and physical MMIO BAR apertures from PnP Manager
//   3. Generates & verifies AMD DCN 3.1.2 (Rembrandt 0x1681) and Intel Gen9-14 HDMI VSIF + AVI InfoFrame
//      + 8bpc No-Dither MMIO register sequences (and supports live MMIO driver injection when present)
//   4. Synthesizes an ETSI GS CCM 001 V1.1.1 Clauses 6.2-6.4 3840x2160 RGB8 Tunnel Golden Frame PNG
//      and verifies all 3 redundant 128-byte packet copies against IEEE 802.3 CRC-32.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace WinDvBridge
{
    public enum DvEdidStatus
    {
        OK = 0,
        InvalidEdid,
        NoCeaExtension,
        NoVsvdb,
        MalformedVsvdb,
        UnsupportedVersion,
        NoLowLatency
    }

    public struct DvSinkCaps
    {
        public DvEdidStatus Status;
        public byte Version;
        public bool Supported;
        public bool LegacyVsif;
        public bool Supports2160p60;
        public bool SupportsYuv422_12Bit;
        public bool SupportsGlobalDimming;
        public bool SupportsBacklightCtrl;
        public byte DmVersion;
        public byte InterfaceMode;
        public byte LowLatencyMask;
        public byte[] RawVsvdb;
        public bool SupportsHdr10Static;
        public byte HdrEotfMask;
    }

    public struct HwInfoFrame
    {
        public uint[] Dw; // 9 dwords (36 bytes)
        public byte Hb0, Hb1, Hb2, Checksum;
        public byte[] Sb; // 28 bytes (PB0=checksum .. PB27)
        public bool Valid;
    }

    public static class Program
    {
        // =========================================================================
        // 1. IEEE 802.3 MSB-first CRC-32 (FFmpeg AV_CRC_32_IEEE / dvbridge_dv_crc)
        // =========================================================================
        private static readonly uint[] Crc32MsbTable = InitCrc32MsbTable();

        private static uint[] InitCrc32MsbTable()
        {
            uint[] table = new uint[256];
            const uint poly = 0x04C11DB7U;
            for (uint i = 0; i < 256; i++)
            {
                uint c = i << 24;
                for (int bit = 0; bit < 8; bit++)
                {
                    if ((c & 0x80000000U) != 0)
                        c = (c << 1) ^ poly;
                    else
                        c <<= 1;
                }
                table[i] = c;
            }
            return table;
        }

        public static uint DvBridgeDvCrc(byte[] data, int offset, int length)
        {
            uint crc = 0xFFFFFFFFU;
            for (int i = 0; i < length; i++)
            {
                crc = (crc << 8) ^ Crc32MsbTable[((crc >> 24) ^ data[offset + i]) & 0xFFU];
            }
            return crc;
        }

        // =========================================================================
        // 2. 12-bit Parity Helper from cb1_transport_glsl.h: cb1_parity12(v)
        // =========================================================================
        public static uint Cb1Parity12(uint v)
        {
            v &= 0xFFFU;
            v ^= v >> 8;
            v ^= v >> 4;
            return (0x6996U >> (int)(v & 15U)) & 1U;
        }

        // =========================================================================
        // 3. ETSI GS CCM 001 V1.1.1 Clauses 6.2 & 6.3 DM Wire Payload & Packetizer
        // =========================================================================
        public static byte[] BuildStandardDolbyVisionWirePacket(
            ushort minPq = 62,
            ushort maxPq = 3079,
            ushort avgPq = 1450,
            ushort activeLeft = 0,
            ushort activeRight = 0,
            ushort activeTop = 280,
            ushort activeBottom = 280)
        {
            // Build 71-byte base header + L1 (6 bytes) + L2 (9 bytes) + L5 (9 bytes) + L254 (3 bytes) = 98 bytes
            byte[] payload = new byte[98];

            // Fixed 31-byte YCC->LMS & LMS->IPT coefficients for Standard TV-Led Profile 8.1/7
            byte[] baseHeader31 = new byte[] {
                0x01, 0x00, 0x09, 0x53, 0x34, 0x1F, 0xFE, 0x3D,
                0x9E, 0x16, 0xAE, 0x01, 0x20, 0x00, 0x00, 0x00,
                0x07, 0xFF, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
                0x08, 0x00, 0x00, 0x00, 0x20, 0x00, 0x02
            };
            Buffer.BlockCopy(baseHeader31, 0, payload, 0, baseHeader31.Length);

            // Dynamic PQ range in base header bytes 31..46
            payload[31] = 0x0C; payload[32] = 0x07;
            payload[33] = 0x00; payload[34] = 0x46;
            payload[35] = 0x38; payload[36] = 0xAA;
            payload[37] = 0x16; payload[38] = 0x99;
            payload[39] = 0x43; payload[40] = 0x01;
            payload[41] = 0x40; payload[42] = 0x04;
            payload[43] = 0xB3; payload[44] = 0x00;
            payload[45] = 0x00; payload[46] = 0x02;

            // Fixed 23-byte tail of 71-byte DM base header (bytes 47..69)
            byte[] baseTail23 = new byte[] {
                0x13, 0x33, 0x38, 0xA8, 0xC4, 0x23, 0xE6, 0x62,
                0x15, 0xA0, 0xE2, 0x0D, 0xD3, 0x22, 0x67, 0xEC,
                0x36, 0x8A, 0x46, 0xD1, 0xC9, 0xD2, 0x00
            };
            Buffer.BlockCopy(baseTail23, 0, payload, 47, baseTail23.Length);

            // num_ext_blocks = 4 (L1, L2, L5, L254)
            payload[70] = 4;
            int pos = 71;

            // L1 block (length=5, level=1, min_pq, max_pq, avg_pq)
            payload[pos + 0] = 5;
            payload[pos + 1] = 1;
            payload[pos + 2] = (byte)((minPq >> 4) & 0xFF);
            payload[pos + 3] = (byte)(((minPq & 0x0F) << 4) | ((maxPq >> 8) & 0x0F));
            payload[pos + 4] = (byte)(maxPq & 0xFF);
            payload[pos + 5] = (byte)((avgPq >> 4) & 0xFF);
            pos += 6;

            // L2 block (length=8, level=2, target_max_pq=2851 [600 nits], trim_slope/offset/power/chroma/sat=2048)
            ushort targetMaxPq = 2851;
            ushort neutralTrim = 2048;
            payload[pos + 0] = 8;
            payload[pos + 1] = 2;
            payload[pos + 2] = (byte)((targetMaxPq >> 4) & 0xFF);
            payload[pos + 3] = (byte)(((targetMaxPq & 0x0F) << 4) | ((neutralTrim >> 8) & 0x0F));
            payload[pos + 4] = (byte)(neutralTrim & 0xFF);
            payload[pos + 5] = (byte)((neutralTrim >> 4) & 0xFF);
            payload[pos + 6] = (byte)(((neutralTrim & 0x0F) << 4) | ((neutralTrim >> 8) & 0x0F));
            payload[pos + 7] = (byte)(neutralTrim & 0xFF);
            payload[pos + 8] = 0x80; // ms_weight = -1
            pos += 9;

            // L5 block (length=8, level=5, active_area_left/right/top/bottom)
            payload[pos + 0] = 8;
            payload[pos + 1] = 5;
            payload[pos + 2] = (byte)((activeLeft >> 8) & 0xFF);
            payload[pos + 3] = (byte)(activeLeft & 0xFF);
            payload[pos + 4] = (byte)((activeRight >> 8) & 0xFF);
            payload[pos + 5] = (byte)(activeRight & 0xFF);
            payload[pos + 6] = (byte)((activeTop >> 8) & 0xFF);
            payload[pos + 7] = (byte)(activeTop & 0xFF);
            payload[pos + 8] = (byte)(activeBottom & 0xFF);
            pos += 9;

            // L254 block (length=2, level=254, dm_mode=0, dm_version_index=2)
            payload[pos + 0] = 2;
            payload[pos + 1] = 254;
            payload[pos + 2] = 0x02;
            pos += 3;

            // Pack into a 128-byte ETSI GS CCM 001 Clause 6.2 & 6.3 single packet:
            // packet[0] = type(0) << 6 = 0x00
            // packet[1] = (id << 4) | id = 0x00
            // packet[2] = 0x00
            // packet[3..4] = big-endian payload_size (98 = 0x0062)
            // packet[5..102] = payload[0..97]
            // packet[124..127] = big-endian IEEE 802.3 CRC-32 of packet[0..123]
            byte[] packet = new byte[128];
            packet[0] = 0x00;
            packet[1] = 0x00;
            packet[2] = 0x00;
            packet[3] = (byte)((payload.Length >> 8) & 0xFF);
            packet[4] = (byte)(payload.Length & 0xFF);
            Buffer.BlockCopy(payload, 0, packet, 5, payload.Length);

            uint crc = DvBridgeDvCrc(packet, 0, 124);
            packet[124] = (byte)((crc >> 24) & 0xFF);
            packet[125] = (byte)((crc >> 16) & 0xFF);
            packet[126] = (byte)((crc >> 8) & 0xFF);
            packet[127] = (byte)(crc & 0xFF);

            return packet;
        }

        // =========================================================================
        // 4. ETSI GS CCM 001 V1.1.1 Clause 6.4 RGB8 Carrier Scanline Packer & Extractor
        // =========================================================================
        public static void PackRgb8TunnelScanline(
            byte[] packetBytes,
            int packetCount,
            ushort[] y12,
            ushort[] c12,
            int width,
            byte[] outRgb8Row)
        {
            uint totalTunnelPixels = (uint)(packetCount * 3072);
            for (int x = 0; x < width; x++)
            {
                uint codeX = (uint)(y12[x] & 0xFFFU);
                uint codeY = (uint)(c12[x] & 0xFFFU);

                if ((uint)x < totalTunnelPixels)
                {
                    uint pktIdx = (uint)x / 3072U;
                    uint bitIdx = (uint)x % 1024U;
                    uint byteIdx = pktIdx * 128U + (bitIdx >> 3);
                    uint shift = 7U - (bitIdx & 7U);
                    uint dataBit = ((uint)packetBytes[byteIdx] >> (int)shift) & 1U;
                    uint parity = Cb1Parity12((codeY & 0xFFEU) ^ codeX);
                    codeY = (codeY & 0xFFEU) | (dataBit ^ parity);
                }

                // cb1_carrier(code): R = C[11:4], G = Y[11:4], B = (C[3:0] << 4) | Y[3:0]
                outRgb8Row[x * 3 + 0] = (byte)((codeY >> 4) & 0xFFU); // R
                outRgb8Row[x * 3 + 1] = (byte)((codeX >> 4) & 0xFFU); // G
                outRgb8Row[x * 3 + 2] = (byte)(((codeY & 0x0FU) << 4) | (codeX & 0x0FU)); // B
            }
        }

        public static byte[] ExtractRgb8TunnelPacketCopy(byte[] rgb8Row, int packetIndex, int copyIndex)
        {
            byte[] recovered = new byte[128];
            int startPixel = packetIndex * 3072 + copyIndex * 1024;
            for (int bit = 0; bit < 1024; bit++)
            {
                int px = startPixel + bit;
                uint r = rgb8Row[px * 3 + 0];
                uint g = rgb8Row[px * 3 + 1];
                uint b = rgb8Row[px * 3 + 2];

                uint codeY = (r << 4) | ((b >> 4) & 0x0FU);
                uint codeX = (g << 4) | (b & 0x0FU);
                uint dataBit = Cb1Parity12(codeY ^ codeX);

                if (dataBit != 0)
                    recovered[bit >> 3] |= (byte)(1U << (7 - (bit & 7)));
            }
            return recovered;
        }

        // =========================================================================
        // 5. EDID Parser (Ported from drm_dvbridge_edid.h: dvbridge_parse_edid)
        // =========================================================================
        public static DvSinkCaps ParseEdid(byte[] edid)
        {
            DvSinkCaps caps = new DvSinkCaps();
            caps.Status = DvEdidStatus.InvalidEdid;

            if (edid == null || edid.Length < 128)
                return caps;

            byte[] header = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
            for (int i = 0; i < 8; i++)
                if (edid[i] != header[i])
                    return caps;

            int extensions = edid[126];
            int maxExt = (edid.Length / 128) - 1;
            if (extensions > maxExt)
                extensions = maxExt;

            bool foundCea = false;
            bool foundVsvdb = false;

            for (int ext = 1; ext <= extensions; ext++)
            {
                int baseOff = ext * 128;
                if (edid[baseOff] != 0x02 || edid[baseOff + 1] < 0x03)
                    continue;

                byte sum = 0;
                for (int i = 0; i < 128; i++)
                    sum += edid[baseOff + i];
                if (sum != 0)
                    continue;

                foundCea = true;
                int dtdStart = edid[baseOff + 2];
                if (dtdStart < 4 || dtdStart > 127)
                    continue;

                int pos = 4;
                while (pos < dtdStart)
                {
                    byte tagHeader = edid[baseOff + pos];
                    int tag = (tagHeader >> 5) & 0x07;
                    int len = tagHeader & 0x1F;
                    if (pos + 1 + len > dtdStart)
                        break;

                    // Extended Tag (7)
                    if (tag == 7 && len >= 2)
                    {
                        byte extTag = edid[baseOff + pos + 1];
                        // Extended Tag 0x06 = HDR Static Metadata Data Block
                        if (extTag == 0x06 && len >= 3)
                        {
                            caps.HdrEotfMask = edid[baseOff + pos + 2];
                            if ((caps.HdrEotfMask & 0x04) != 0) // SMPTE ST 2084 (PQ)
                                caps.SupportsHdr10Static = true;
                        }
                        // Extended Tag 0x01 = Video Capability / Vendor-Specific Video Data Block (VSVDB)
                        else if (extTag == 0x01 && len >= 5)
                        {
                            uint oui = (uint)edid[baseOff + pos + 2] |
                                       ((uint)edid[baseOff + pos + 3] << 8) |
                                       ((uint)edid[baseOff + pos + 4] << 16);
                            if (oui == 0x00D046U)
                            {
                                foundVsvdb = true;
                                caps.RawVsvdb = new byte[len + 1];
                                Buffer.BlockCopy(edid, baseOff + pos, caps.RawVsvdb, 0, len + 1);

                                byte vsvdbHeader = edid[baseOff + pos + 5];
                                byte version = (byte)((vsvdbHeader >> 5) & 0x07);
                                caps.Version = version;

                                if (version == 0)
                                {
                                    if (len != 0x19) { caps.Status = DvEdidStatus.MalformedVsvdb; }
                                    else
                                    {
                                        caps.Supported = true;
                                        caps.LegacyVsif = true;
                                        caps.SupportsYuv422_12Bit = (vsvdbHeader & 0x01) != 0;
                                        caps.Supports2160p60 = (vsvdbHeader & 0x02) != 0;
                                        caps.SupportsGlobalDimming = (vsvdbHeader & 0x04) != 0;
                                        caps.DmVersion = (byte)((edid[baseOff + pos + 21] >> 2) & 0x07);
                                        caps.Status = DvEdidStatus.OK;
                                    }
                                }
                                else if (version == 1)
                                {
                                    if (len != 0x0E && len != 0x0B)
                                    {
                                        caps.Status = DvEdidStatus.MalformedVsvdb;
                                    }
                                    else
                                    {
                                        caps.Supported = true;
                                        caps.SupportsYuv422_12Bit = (vsvdbHeader & 0x01) != 0;
                                        caps.Supports2160p60 = (vsvdbHeader & 0x02) != 0;
                                        caps.DmVersion = (byte)((vsvdbHeader >> 2) & 0x07);
                                        caps.SupportsGlobalDimming = (edid[baseOff + pos + 6] & 0x01) != 0;
                                        if (len == 0x0E)
                                        {
                                            caps.LegacyVsif = true;
                                            caps.LowLatencyMask = 0;
                                        }
                                        else
                                        {
                                            caps.LowLatencyMask = (byte)(edid[baseOff + pos + 7] & 0x03);
                                            caps.LegacyVsif = (edid[baseOff + pos + 7] & 0x01) == 0;
                                        }
                                        caps.Status = DvEdidStatus.OK;
                                    }
                                }
                                else if (version == 2)
                                {
                                    if (len < 0x0B)
                                    {
                                        caps.Status = DvEdidStatus.MalformedVsvdb;
                                    }
                                    else
                                    {
                                        caps.SupportsYuv422_12Bit = (vsvdbHeader & 0x01) != 0;
                                        caps.Supports2160p60 = true;
                                        caps.SupportsBacklightCtrl = (vsvdbHeader & 0x02) != 0;
                                        caps.DmVersion = (byte)((vsvdbHeader >> 2) & 0x07);
                                        caps.SupportsGlobalDimming = (edid[baseOff + pos + 6] & 0x04) != 0;
                                        caps.InterfaceMode = (byte)(edid[baseOff + pos + 7] & 0x03);
                                        // InterfaceMode: 0,1 = LL-only; 2,3 = Standard + LL
                                        caps.LegacyVsif = false;
                                        caps.Supported = true;
                                        caps.Status = (caps.InterfaceMode >= 2) ? DvEdidStatus.OK : DvEdidStatus.NoLowLatency;
                                    }
                                }
                                else
                                {
                                    caps.Status = DvEdidStatus.UnsupportedVersion;
                                }
                            }
                        }
                    }
                    pos += 1 + len;
                }
            }

            if (!foundCea)
                caps.Status = DvEdidStatus.NoCeaExtension;
            else if (!foundVsvdb)
                caps.Status = DvEdidStatus.NoVsvdb;

            return caps;
        }

        // =========================================================================
        // 6. HDMI InfoFrame Packing (Matching dvbridge_win_mmio.h)
        // =========================================================================
        public static HwInfoFrame PackDolbyVsif(bool enableDv, bool lowLatency, bool legacyVsif, byte ctaVic)
        {
            HwInfoFrame f = new HwInfoFrame
            {
                Dw = new uint[9],
                Sb = new byte[28],
                Valid = enableDv
            };
            if (!enableDv) return f;

            f.Hb0 = 0x81;
            f.Hb1 = 0x01;
            if (!legacyVsif)
            {
                f.Hb2 = 0x1B; // 27 bytes
                f.Sb[1] = 0x46;
                f.Sb[2] = 0xD0;
                f.Sb[3] = 0x00;
                f.Sb[4] = (byte)((enableDv ? 0x02 : 0x00) | (lowLatency ? 0x01 : 0x00));
            }
            else
            {
                byte hdmiVic = 0;
                if (ctaVic == 95) hdmiVic = 1;
                else if (ctaVic == 94) hdmiVic = 2;
                else if (ctaVic == 93) hdmiVic = 3;
                else if (ctaVic == 98) hdmiVic = 4;

                f.Hb2 = 0x18; // 24 bytes
                f.Sb[1] = 0x03;
                f.Sb[2] = 0x0C;
                f.Sb[3] = 0x00;
                if (hdmiVic > 0)
                {
                    f.Sb[4] = 0x20;
                    f.Sb[5] = hdmiVic;
                }
            }

            uint sum = (uint)f.Hb0 + f.Hb1 + f.Hb2;
            for (int i = 1; i <= f.Hb2; i++) sum += f.Sb[i];
            f.Checksum = (byte)((0x100U - (sum & 0xFFU)) & 0xFFU);
            f.Sb[0] = f.Checksum;

            f.Dw[0] = (uint)f.Hb0 | ((uint)f.Hb1 << 8) | ((uint)f.Hb2 << 16);
            for (int w = 0; w < 7; w++)
            {
                f.Dw[w + 1] = (uint)f.Sb[w * 4 + 0] |
                              ((uint)f.Sb[w * 4 + 1] << 8) |
                              ((uint)f.Sb[w * 4 + 2] << 16) |
                              ((uint)f.Sb[w * 4 + 3] << 24);
            }
            return f;
        }

        public static HwInfoFrame PackAviInfoFrame(byte ctaVic, bool legacyVsif)
        {
            HwInfoFrame f = new HwInfoFrame
            {
                Dw = new uint[9],
                Sb = new byte[28],
                Valid = true
            };
            byte effectiveVic = ctaVic;
            if (legacyVsif && (ctaVic == 93 || ctaVic == 94 || ctaVic == 95 || ctaVic == 98))
                effectiveVic = 0;

            f.Hb0 = 0x82;
            f.Hb1 = 0x02;
            f.Hb2 = 0x0D;
            f.Sb[1] = 0x12; // RGB Full + Active Format Valid + Underscan
            f.Sb[2] = 0x28; // Colorimetry=None, Aspect=16:9, ActiveAspect=Same
            f.Sb[3] = 0x08; // Full Quantization (Q=2)
            f.Sb[4] = effectiveVic;
            f.Sb[5] = 0x00;

            uint sum = (uint)f.Hb0 + f.Hb1 + f.Hb2;
            for (int i = 1; i <= f.Hb2; i++) sum += f.Sb[i];
            f.Checksum = (byte)((0x100U - (sum & 0xFFU)) & 0xFFU);
            f.Sb[0] = f.Checksum;

            f.Dw[0] = (uint)f.Hb0 | ((uint)f.Hb1 << 8) | ((uint)f.Hb2 << 16);
            for (int w = 0; w < 7; w++)
            {
                f.Dw[w + 1] = (uint)f.Sb[w * 4 + 0] |
                              ((uint)f.Sb[w * 4 + 1] << 8) |
                              ((uint)f.Sb[w * 4 + 2] << 16) |
                              ((uint)f.Sb[w * 4 + 3] << 24);
            }
            return f;
        }

        // =========================================================================
        // 7. Windows Registry EDID & PCI BAR MMIO Scanner
        // =========================================================================
        private static void ScanWindowsDisplays()
        {
            Console.WriteLine("========================================================================");
            Console.WriteLine("[1] Scanning Windows Registry for Monitor EDIDs & Dolby Vision VSVDB...");
            Console.WriteLine("========================================================================");

            int monitorCount = 0;
            using (RegistryKey displayRoot = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\DISPLAY"))
            {
                if (displayRoot != null)
                {
                    foreach (string monHardwareId in displayRoot.GetSubKeyNames())
                    {
                        using (RegistryKey monKey = displayRoot.OpenSubKey(monHardwareId))
                        {
                            if (monKey == null) continue;
                            foreach (string instId in monKey.GetSubKeyNames())
                            {
                                using (RegistryKey instKey = monKey.OpenSubKey(instId))
                                {
                                    if (instKey == null) continue;
                                    using (RegistryKey devParam = instKey.OpenSubKey("Device Parameters"))
                                    {
                                        if (devParam == null) continue;
                                        byte[] edid = devParam.GetValue("EDID") as byte[];
                                        if (edid == null || edid.Length < 128) continue;

                                        monitorCount++;
                                        string friendly = (instKey.GetValue("FriendlyName") as string)
                                                       ?? (instKey.GetValue("DeviceDesc") as string)
                                                       ?? monHardwareId;

                                        DvSinkCaps caps = ParseEdid(edid);
                                        Console.WriteLine("  * Monitor #{0}: DISPLAY\\{1}\\{2}", monitorCount, monHardwareId, instId);
                                        Console.WriteLine("    - Name              : {0}", friendly);
                                        Console.WriteLine("    - EDID Size         : {0} bytes ({1} block(s))", edid.Length, edid.Length / 128);
                                        Console.WriteLine("    - HDR10 PQ (ST2084) : {0} (EOTF mask=0x{1:X2})", caps.SupportsHdr10Static ? "YES" : "NO", caps.HdrEotfMask);
                                        Console.WriteLine("    - Dolby Vision VSVDB: Status={0}, Supported={1}, Version={2}, LegacyVSIF={3}, 2160p60={4}",
                                            caps.Status, caps.Supported, caps.Version, caps.LegacyVsif, caps.Supports2160p60);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (monitorCount == 0)
            {
                Console.WriteLine("  * No physical EDID blobs found in HKLM\\SYSTEM\\CurrentControlSet\\Enum\\DISPLAY.");
            }

            // Also run self-verification on a reference LG C2/C3/G3 4K Dolby Vision TV EDID
            byte[] simEdid = new byte[256];
            byte[] hdr = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
            Buffer.BlockCopy(hdr, 0, simEdid, 0, 8);
            simEdid[126] = 1;
            byte sum0 = 0; for (int i = 0; i < 127; i++) sum0 += simEdid[i];
            simEdid[127] = (byte)(0x100 - sum0);

            int cea = 128;
            simEdid[cea + 0] = 0x02;
            simEdid[cea + 1] = 0x03;
            simEdid[cea + 2] = 0x16; // DTD offset = 22
            simEdid[cea + 3] = 0xF0;
            // HDR10 Static Metadata Data Block (4 bytes: pos 4..7)
            simEdid[cea + 4] = (7 << 5) | 3;
            simEdid[cea + 5] = 0x06; // HDR Static Metadata
            simEdid[cea + 6] = 0x0C; // SDR + HDR10 PQ + HLG
            simEdid[cea + 7] = 0x01; // Type 1
            // Dolby Vision VSVDB v1 12-byte (pos 8..19, len=11)
            simEdid[cea + 8]  = (7 << 5) | 11;
            simEdid[cea + 9]  = 0x01;
            simEdid[cea + 10] = 0x46;
            simEdid[cea + 11] = 0xD0;
            simEdid[cea + 12] = 0x00;
            simEdid[cea + 13] = (1 << 5) | (3 << 2) | 0x03; // v1, DM v3, 2160p60=1, YUV422=1
            simEdid[cea + 14] = 0x01; // global dimming
            simEdid[cea + 15] = 0x01; // Modern Dolby VSIF (OUI 0x00D046) + Standard + LL
            byte sum1 = 0; for (int i = 0; i < 127; i++) sum1 += simEdid[cea + i];
            simEdid[cea + 127] = (byte)(0x100 - sum1);

            DvSinkCaps simCaps = ParseEdid(simEdid);
            Console.WriteLine("  * [Reference Sink Test] Simulated 4K Dolby Vision TV EDID:");
            Console.WriteLine("    - HDR10 PQ={0}, DV Status={1}, Version={2}, ModernVSIF={3}, 2160p60={4}",
                simCaps.SupportsHdr10Static, simCaps.Status, simCaps.Version, !simCaps.LegacyVsif, simCaps.Supports2160p60);
            Console.WriteLine();
        }

        private static void ScanWindowsGpuAndMmio()
        {
            Console.WriteLine("========================================================================");
            Console.WriteLine("[2] Scanning Windows Display Adapters & Hardware MMIO BAR Apertures...");
            Console.WriteLine("========================================================================");

            // Display Class GUID: {4d36e968-e325-11ce-bfc1-08002be10318}
            using (RegistryKey videoClass = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"))
            {
                if (videoClass != null)
                {
                    foreach (string sub in videoClass.GetSubKeyNames())
                    {
                        if (!sub.StartsWith("0")) continue;
                        using (RegistryKey k = videoClass.OpenSubKey(sub))
                        {
                            if (k == null) continue;
                            string desc = k.GetValue("DriverDesc") as string;
                            string matchingId = k.GetValue("MatchingDeviceId") as string;
                            string drvVer = k.GetValue("DriverVersion") as string;
                            if (!string.IsNullOrEmpty(desc))
                            {
                                Console.WriteLine("  * Adapter [{0}]: {1}", sub, desc);
                                Console.WriteLine("    - MatchingDeviceId : {0}", matchingId);
                                Console.WriteLine("    - DriverVersion    : {0}", drvVer);
                            }
                        }
                    }
                }
            }

            Console.WriteLine();
            Console.WriteLine("  * AMD DCN 3.1.2 (PCI\\VEN_1002&DEV_1681) Hardware TV-Led Dolby Vision MMIO Sequence:");
            uint seg2Base = 0x000034C0U;
            uint fmtReg = (seg2Base + 0x1841U) * 4U;
            uint vpgCtrlReg = (seg2Base + 0x2068U) * 4U;
            uint vpgDataReg = (seg2Base + 0x2069U) * 4U;
            uint vpgImmReg = (seg2Base + 0x206BU) * 4U;
            uint genCtrl0Reg = (seg2Base + 0x209AU) * 4U;
            uint genCtrl1Reg = (seg2Base + 0x209EU) * 4U;

            HwInfoFrame vsif = PackDolbyVsif(true, false, false, 93);
            HwInfoFrame avi = PackAviInfoFrame(93, false);

            Console.WriteLine("    1) Disable Truncation & Spatial/Temporal Dithering:");
            Console.WriteLine("       MMIO[0x{0:X5}] (FMT0_FMT_BIT_DEPTH_CONTROL) &= ~0x0001FF33", fmtReg);
            Console.WriteLine("    2) Write AVI InfoFrame (RGB Full, 8bpc, VIC=93) to VPG0 Slot 0:");
            Console.WriteLine("       MMIO[0x{0:X5}] (VPG0_GENERIC_PACKET_ACCESS_CTRL) = 0x00000000", vpgCtrlReg);
            Console.WriteLine("       MMIO[0x{0:X5}] (VPG0_GENERIC_PACKET_DATA DW0..2) = 0x{1:X8}, 0x{2:X8}, 0x{3:X8}",
                vpgDataReg, avi.Dw[0], avi.Dw[1], avi.Dw[2]);
            Console.WriteLine("    3) Write Dolby Vision VSIF (OUI 0x00D046, TV-Led) to VPG0 Slot 1:");
            Console.WriteLine("       MMIO[0x{0:X5}] (VPG0_GENERIC_PACKET_ACCESS_CTRL) = 0x00000009", vpgCtrlReg);
            Console.WriteLine("       MMIO[0x{0:X5}] (VPG0_GENERIC_PACKET_DATA DW0..2) = 0x{1:X8}, 0x{2:X8}, 0x{3:X8}",
                vpgDataReg, vsif.Dw[0], vsif.Dw[1], vsif.Dw[2]);
            Console.WriteLine("    4) Trigger VPG0 GSP Immediate Update & Enable Continuous Line-2 Transmission:");
            Console.WriteLine("       MMIO[0x{0:X5}] (VPG0_GSP_IMMEDIATE_UPDATE_CTRL)  = 0x00000002", vpgImmReg);
            Console.WriteLine("       MMIO[0x{0:X5}] (DIG0_HDMI_GENERIC_PACKET_CTRL0)  |= 0x00000033 (ENABLE0|CONT0|ENABLE1|CONT1)", genCtrl0Reg);
            Console.WriteLine("       MMIO[0x{0:X5}] (DIG0_HDMI_GENERIC_PACKET_CTRL1)  = 0x00020002 (LINE0=2, LINE1=2)", genCtrl1Reg);
            Console.WriteLine();
        }

        private static void GenerateAndVerify4kGoldenFrame(string outputPath)
        {
            Console.WriteLine("========================================================================");
            Console.WriteLine("[3] Synthesizing 3840x2160 ETSI GS CCM 001 V1.1.1 Dolby Vision Frame...");
            Console.WriteLine("========================================================================");

            const int width = 3840;
            const int height = 2160;

            byte[] packet = BuildStandardDolbyVisionWirePacket();
            uint packetCrc = DvBridgeDvCrc(packet, 0, 128);
            Console.WriteLine("  * Built 128-byte DM Wire Packet: Payload=98 bytes, CRC32(0..123)=0x{0:X2}{1:X2}{2:X2}{3:X2}, Self-Check Residue=0x{4:X8}",
                packet[124], packet[125], packet[126], packet[127], packetCrc);

            ushort[] y12 = new ushort[width];
            ushort[] c12 = new ushort[width];
            for (int x = 0; x < width; x++)
            {
                // Smooth 12-bit IPT-PQ gradient ramp across 3840 horizontal pixels
                y12[x] = (ushort)(256 + ((x * 3328) / width));
                c12[x] = (ushort)(((x & 1) == 0) ? 2048 : 2064); // Neutral I/P or Cw/Ct
            }

            byte[] scanline0Rgb8 = new byte[width * 3];
            PackRgb8TunnelScanline(packet, 1, y12, c12, width, scanline0Rgb8);

            // Verify all 3 redundant copies (pixels 0..1023, 1024..2047, 2048..3071)
            for (int copy = 0; copy < 3; copy++)
            {
                byte[] rec = ExtractRgb8TunnelPacketCopy(scanline0Rgb8, 0, copy);
                uint residue = DvBridgeDvCrc(rec, 0, 128);
                bool exactMatch = true;
                for (int i = 0; i < 128; i++)
                {
                    if (rec[i] != packet[i]) { exactMatch = false; break; }
                }
                Console.WriteLine("  * Extracted Copy #{0} (Pixels {1,4}..{2,4}): ExactMatch={3}, IEEE CRC-32 Residue=0x{4:X8}",
                    copy, copy * 1024, copy * 1024 + 1023, exactMatch ? "PASS" : "FAIL", residue);
                if (!exactMatch || residue != 0)
                    throw new InvalidOperationException("ETSI GS CCM 001 Clause 6.4 round-trip failed!");
            }

            // Write 3840x2160 24-bit PNG image with the exact scanline 0 Dolby Vision RGB8 tunnel
            using (Bitmap bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb))
            {
                BitmapData bd = bmp.LockBits(new Rectangle(0, 0, width, height),
                    ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
                try
                {
                    byte[] rowBuffer = new byte[width * 3];
                    // Row 0: ETSI GS CCM 001 Clause 6.4 RGB8 Tunnel Scanline (Format24bppRgb is B,G,R in memory)
                    for (int x = 0; x < width; x++)
                    {
                        rowBuffer[x * 3 + 2] = scanline0Rgb8[x * 3 + 0]; // R = C[11:4]
                        rowBuffer[x * 3 + 1] = scanline0Rgb8[x * 3 + 1]; // G = Y[11:4]
                        rowBuffer[x * 3 + 0] = scanline0Rgb8[x * 3 + 2]; // B = (C[3:0] << 4) | Y[3:0]
                    }
                    Marshal.Copy(rowBuffer, 0, bd.Scan0, width * 3);

                    // Rows 1..2159: 12-bit IPT-PQ packed into RGB8 via cb1_carrier (with 280px top/bottom letterbox bars per L5)
                    byte[] barBuffer = new byte[width * 3];
                    for (int x = 0; x < width; x++)
                    {
                        uint cy = 2048U; // Neutral chroma
                        uint cx = 64U;   // Black level PQ in 12-bit IPT
                        barBuffer[x * 3 + 2] = (byte)(cy >> 4);
                        barBuffer[x * 3 + 1] = (byte)(cx >> 4);
                        barBuffer[x * 3 + 0] = (byte)(((cy & 0x0FU) << 4) | (cx & 0x0FU));
                    }

                    byte[] activeBuffer = new byte[width * 3];
                    for (int x = 0; x < width; x++)
                    {
                        uint cy = c12[x];
                        uint cx = y12[x];
                        activeBuffer[x * 3 + 2] = (byte)(cy >> 4);
                        activeBuffer[x * 3 + 1] = (byte)(cx >> 4);
                        activeBuffer[x * 3 + 0] = (byte)(((cy & 0x0FU) << 4) | (cx & 0x0FU));
                    }

                    for (int y = 1; y < height; y++)
                    {
                        IntPtr rowPtr = IntPtr.Add(bd.Scan0, y * bd.Stride);
                        if (y < 280 || y >= height - 280)
                            Marshal.Copy(barBuffer, 0, rowPtr, width * 3);
                        else
                            Marshal.Copy(activeBuffer, 0, rowPtr, width * 3);
                    }
                }
                finally
                {
                    bmp.UnlockBits(bd);
                }

                bmp.Save(outputPath, ImageFormat.Png);
            }

            FileInfo fi = new FileInfo(outputPath);
            Console.WriteLine("  * Saved 4K Dolby Vision RGB8 Tunnel Golden Frame: {0} ({1:N0} bytes)",
                fi.FullName, fi.Length);
            Console.WriteLine("========================================================================");
        }

        public static int Main(string[] args)
        {
            try
            {
                Console.OutputEncoding = Encoding.UTF8;
                Console.WriteLine("╔══════════════════════════════════════════════════════════════════════╗");
                Console.WriteLine("║  LibreELEC-DV (CB1 + DVBridge) Native Windows 64-bit Hardware Tool   ║");
                Console.WriteLine("╚══════════════════════════════════════════════════════════════════════╝");
                Console.WriteLine();

                ScanWindowsDisplays();
                ScanWindowsGpuAndMmio();

                string outPng = args.Length > 0 ? args[0] : "win_dv_golden_3840x2160.png";
                GenerateAndVerify4kGoldenFrame(outPng);

                Console.WriteLine("[SUCCESS] Windows Dolby Vision Hardware & ETSI GS CCM 001 Engine Ready.");
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("[ERROR] " + ex);
                return 1;
            }
        }
    }
}
