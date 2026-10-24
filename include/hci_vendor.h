/**-------------------------------------------------------------------------
@file	hci_vendor.h
@brief	I-SYST vendor commands of HciController: the identity of the dongle
		and the crypto of the controller for a host that has none of its own.
		Routed by the packet bridge ahead of the SoftDevice Controller, like
		the diagnostics of hci_controller.h, on every host transport.
@author	Nguyen Hoan Hoang
@date	October 2026
@license MPL-2.0, (c) 2026 I-SYST inc. See LICENSE.
----------------------------------------------------------------------------*/
#ifndef HCI_VENDOR_H
#define HCI_VENDOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The opcodes follow the three HciController diagnostics (0xFFF0 to 0xFFF2),
 * OGF 0x3F. Every one answers with a Command Complete whose first return
 * octet is the HCI status; the layouts below are what follows it.
 *
 * Read Static Address. No parameters. Returns the static random address of
 * the part, the one in its factory registers with the two top bits set,
 * least significant octet first. HCI_Read_BD_ADDR keeps answering zeros,
 * because that one is the public address and the part has none; a host that
 * put the factory value there would advertise it as public. A target with
 * no factory address answers Unsupported Feature.
 *   return: addr[6]
 */
#define HCI_VENDOR_OPCODE_READ_STATIC_ADDR      0xFFF3U

/*
 * Crypto Supported. No parameters. Returns the bitmap of the crypto commands
 * this build answers, so a host asks once and chooses its path.
 *   return: bitmap, 4 octets little endian, HCI_VENDOR_CRYPTO_* bits
 */
#define HCI_VENDOR_OPCODE_CRYPTO_SUPPORTED      0xFFF4U
#define HCI_VENDOR_CRYPTO_AES_CMAC              (1UL << 0)
#define HCI_VENDOR_CRYPTO_AES_CCM               (1UL << 1)
#define HCI_VENDOR_CRYPTO_RANDOM                (1UL << 2)
#define HCI_VENDOR_CRYPTO_SHA256                (1UL << 3)
#define HCI_VENDOR_CRYPTO_HMAC_SHA256           (1UL << 4)
#define HCI_VENDOR_CRYPTO_ECDSA_P256_SIGN       (1UL << 5)
#define HCI_VENDOR_CRYPTO_ECDSA_P256_VERIFY     (1UL << 6)

/*
 * AES-CMAC (RFC 4493), one call for a whole message. The SMP functions
 * f4, f5, f6, g2, h6, h7 and the MIC of Encrypted Advertising Data are each
 * one of these instead of a round trip per AES block.
 *   params: key[16], len, msg[len]
 *   return: mac[16]
 */
#define HCI_VENDOR_OPCODE_AES_CMAC              0xFFF5U
#define HCI_VENDOR_AES_CMAC_HDR_LEN             17U
#define HCI_VENDOR_AES_CMAC_MSG_MAX             (255U - HCI_VENDOR_AES_CMAC_HDR_LEN)

/*
 * AES-CCM (RFC 3610), one call for a whole packet.
 *   params: flags, key[16], nonce_len, nonce[nonce_len], aad_len, aad[aad_len],
 *           tag_len, msg_len, msg[msg_len], and on decrypt tag[tag_len]
 *   flags:  bit 0 set to decrypt and check the tag, clear to encrypt
 *   return: out[msg_len], and on encrypt tag[tag_len]
 * nonce_len 7 to 13, tag_len even from 4 to 16. A decrypt whose tag does
 * not check answers Authentication Failure and no data.
 */
#define HCI_VENDOR_OPCODE_AES_CCM               0xFFF6U
#define HCI_VENDOR_AES_CCM_FLAG_DECRYPT         0x01U
#define HCI_VENDOR_AES_CCM_NONCE_MIN            7U
#define HCI_VENDOR_AES_CCM_NONCE_MAX            13U

/*
 * Random. Octets from the entropy source the controller runs on.
 *   params: len, 1 to HCI_VENDOR_RETURN_MAX
 *   return: rand[len]
 */
#define HCI_VENDOR_OPCODE_RANDOM                0xFFF7U

/*
 * SHA-256 of a message.
 *   params: len, msg[len]
 *   return: digest[32]
 */
#define HCI_VENDOR_OPCODE_SHA256                0xFFF8U

/*
 * HMAC-SHA-256 of a message.
 *   params: key_len, key[key_len], msg_len, msg[msg_len]
 *   return: mac[32]
 */
#define HCI_VENDOR_OPCODE_HMAC_SHA256           0xFFF9U

/*
 * ECDSA P-256 over a SHA-256 digest. Keys and signature are the raw big
 * endian octets, the public key its X then Y coordinate.
 *   sign   params: private_key[32], hash[32]   return: sig[64]
 *   verify params: public_key[64], hash[32], sig[64]   return: none,
 *          the status is Success or Authentication Failure
 */
#define HCI_VENDOR_OPCODE_ECDSA_P256_SIGN       0xFFFAU
#define HCI_VENDOR_OPCODE_ECDSA_P256_VERIFY     0xFFFBU

/* Return parameters a Command Complete carries after its status octet. */
#define HCI_VENDOR_RETURN_MAX                   251U

/**
 * @brief	Whether an opcode is one of these commands.
 */
bool HciVendorIsCommand(uint16_t Opcode);

/**
 * @brief	Answer one of these commands.
 *
 * Builds the complete Command Complete Event (event code, length, number of
 * HCI command packets, opcode, status, return parameters) into pEvent.
 *
 * @param	pPacket		The HCI command packet, header included
 * @param	PacketLen	Its length
 * @param	pEvent		Event buffer
 * @param	Capacity	Its size
 *
 * @return	Event length in octets, 0 when pPacket is not one of these
 * 			commands or the event does not fit Capacity
 */
size_t HciVendorCommand(const uint8_t *pPacket, size_t PacketLen,
                        uint8_t *pEvent, size_t Capacity);

/**
 * @brief	Static random address of the part. Defined by the target.
 *
 * The library default answers false and the Read Static Address command
 * then answers Unsupported Feature.
 *
 * @param	Addr	The six octets to fill, least significant first, with the
 * 					two top bits of Addr[5] set
 *
 * @return	true when Addr was filled
 */
bool HciVendorStaticAddr(uint8_t Addr[6]);

#ifdef __cplusplus
}

class RngEngine;

#endif

#endif /* HCI_VENDOR_H */
