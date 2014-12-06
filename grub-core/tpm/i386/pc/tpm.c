/* Begin TCG extension */

/* tpm.c -- tpm module.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2005,2007,2008,2009,2010  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/types.h>
#include <grub/extcmd.h>
#include <grub/mm.h>
#include <grub/dl.h>
#include <grub/crypto.h>
#include <grub/file.h>

#include <grub/machine/tpm.h>
#include <grub/machine/boot.h>
#include <grub/machine/memory.h>

GRUB_MOD_LICENSE ("GPLv3+");


/* TPM_ENTITY_TYPE values */
static const grub_uint16_t TPM_ET_SRK =  0x0004;

/* Reserved Key Handles */
static const grub_uint32_t TPM_KH_SRK = 0x40000000;

/* Ordinals */
static const grub_uint32_t TPM_ORD_OSAP = 0x0000000B;
static const grub_uint32_t TPM_ORD_PcrRead = 0x00000015;
static const grub_uint32_t TPM_ORD_Unseal = 0x00000018;
static const grub_uint32_t TPM_ORD_GetRandom = 0x00000046;
static const grub_uint32_t TPM_ORD_OIAP = 0x0000000A;

static const grub_uint32_t TCG_PCR_EVENT_SIZE = 32;

static const grub_uint16_t TPM_TAG_RQU_AUTH2_COMMAND = 0x00C3;

#define TPM_NONCE_SIZE 20
static const grub_uint32_t TPM_AUTHDATA_SIZE = 20;

static const grub_uint8_t srkAuthData[SHA1_DIGEST_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const grub_uint8_t blobAuthData[SHA1_DIGEST_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* TPM_PCRRead Incoming Operand */
typedef struct {
	grub_uint16_t tag;
	grub_uint32_t paramSize;
	grub_uint32_t ordinal;
	grub_uint32_t pcrIndex;
} __attribute__ ((packed)) PCRReadIncoming;

/* TPM_PCRRead Outgoing Operand */
typedef struct {
	grub_uint16_t tag;
	grub_uint32_t paramSize;
	grub_uint32_t returnCode;
	grub_uint8_t pcr_value[SHA1_DIGEST_SIZE];
} __attribute__ ((packed)) PCRReadOutgoing;

/* TCG_SetMemoryOverwriteRequestBit Input Parameter Block */
typedef struct {
	grub_uint16_t iPBLength;
	grub_uint16_t reserved;
	grub_uint8_t  memoryOverwriteActionBitValue;
} __attribute__ ((packed)) SetMemoryOverwriteRequestBitInputParamBlock;

/* TPM_GetRandom Incoming Operand */
typedef struct {
	grub_uint16_t tag;
	grub_uint32_t paramSize;
	grub_uint32_t ordinal;
	grub_uint32_t bytesRequested;
} __attribute__ ((packed)) GetRandomIncoming;

/* TPM_OIAP Incoming Operand */
typedef struct {
	grub_uint16_t tag;
	grub_uint32_t paramSize;
	grub_uint32_t ordinal;
} __attribute__ ((packed)) OIAP_Incoming;

/* TPM_OIAP Outgoing Operand */
typedef struct {
	grub_uint16_t tag;
	grub_uint32_t paramSize;
	grub_uint32_t returnCode;
	grub_uint32_t authHandle;
	grub_uint8_t  nonceEven[TPM_NONCE_SIZE];
} __attribute__ ((packed)) OIAP_Outgoing;

/* TPM_OSAP Incoming Operand */
typedef struct {
	grub_uint16_t tag;
	grub_uint32_t paramSize;
	grub_uint32_t ordinal;
	grub_uint16_t entityType;
	grub_uint32_t entityValue;
	grub_uint8_t  nonceOddOSAP[TPM_NONCE_SIZE];
} __attribute__ ((packed)) OSAP_Incoming;

/* TPM_OSAP Outgoing Operand */
typedef struct {
	grub_uint16_t tag;
	grub_uint32_t paramSize;
	grub_uint32_t returnCode;
	grub_uint32_t authHandle;
	grub_uint8_t  nonceEven[TPM_NONCE_SIZE];
	grub_uint8_t  nonceEvenOSAP[TPM_NONCE_SIZE];
} __attribute__ ((packed)) OSAP_Outgoing;

typedef struct tdTCG_PCClientPCREventStruc {
	grub_uint32_t pcrIndex;
	grub_uint32_t eventType;
	grub_uint8_t digest[SHA1_DIGEST_SIZE];
	grub_uint32_t eventDataSize;
	grub_uint8_t event[1];
} __attribute__ ((packed)) TCG_PCClientPCREvent;


static grub_err_t
grub_TPM_readpcr( const unsigned long index, grub_uint8_t* result ) {

    CHECK_FOR_NULL_ARGUMENT( result )

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	PassThroughToTPM_InputParamBlock *passThroughInput;
	PCRReadIncoming* pcrReadIncoming;
    grub_uint16_t inputlen = sizeof( *passThroughInput ) - sizeof( passThroughInput->TPMOperandIn ) + sizeof( *pcrReadIncoming );

	PassThroughToTPM_OutputParamBlock *passThroughOutput;
	PCRReadOutgoing* pcrReadOutgoing;
	/* FIXME: Why are these additional +47 bytes needed? */
    grub_uint16_t outputlen = sizeof( *passThroughOutput ) - sizeof( passThroughOutput->TPMOperandOut ) + sizeof( *pcrReadOutgoing ) + 47 ;

	passThroughInput = grub_zalloc( inputlen );
	if( ! passThroughInput ) {
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "readpcr: memory allocation failed" ) );
	}

	passThroughInput->IPBLength = inputlen;
	passThroughInput->OPBLength = outputlen;

	pcrReadIncoming = (void *)passThroughInput->TPMOperandIn;
	pcrReadIncoming->tag = swap16( TPM_TAG_RQU_COMMAND );
	pcrReadIncoming->paramSize = swap32( sizeof( *pcrReadIncoming ) );
	pcrReadIncoming->ordinal = swap32( TPM_ORD_PcrRead );
	pcrReadIncoming->pcrIndex = swap32( (grub_uint32_t) index);

	passThroughOutput = grub_zalloc( outputlen );
	if( ! passThroughOutput ) {
		grub_free( passThroughInput );
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "readpcr: memory allocation failed" ) );
	}

	grub_uint32_t passThroughTo_TPM_ReturnCode;
	grub_err_t err = tcg_passThroughToTPM( passThroughInput, passThroughOutput, &passThroughTo_TPM_ReturnCode );

    if( err != GRUB_ERR_NONE ) {
		grub_free( passThroughInput );
		grub_free( passThroughOutput );
        return err;
	}
	grub_free( passThroughInput );

	pcrReadOutgoing = (void *)passThroughOutput->TPMOperandOut;
	grub_uint32_t tpm_PCRreadReturnCode = swap32( pcrReadOutgoing->returnCode );

	if( tpm_PCRreadReturnCode != TPM_SUCCESS ) {
		grub_free( passThroughOutput );

		if( tpm_PCRreadReturnCode == TPM_BADINDEX ) {
            return grub_error( GRUB_ERR_TPM, N_( "readpcr: bad pcr index" ) );
		}

        return grub_error( GRUB_ERR_TPM, N_( "readpcr: tpm_PCRreadReturnCode: %x" ), tpm_PCRreadReturnCode );
	}

	grub_memcpy( result, pcrReadOutgoing->pcr_value, SHA1_DIGEST_SIZE );

	grub_free( passThroughOutput );
	return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_readpcr( grub_command_t cmd __attribute__ ((unused)), int argc, char **args) {

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	if ( argc == 0 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "index expected" ) );
	}

	if ( argc > 1 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "Too many arguments" ) );
	}

	unsigned long index = grub_strtoul( args[0], NULL, 10 );

	/* if index is invalid */
	if( grub_errno != GRUB_ERR_NONE ) {
        return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "invalid format for index" ) );
	}

	grub_uint8_t result[SHA1_DIGEST_SIZE];
    grub_err_t err = grub_TPM_readpcr( index, &result[0] );

    if( err != GRUB_ERR_NONE ) {
        return err;
    }

	grub_printf( "PCR[%lu]=", index );
	print_sha1( result );
	grub_printf("\n");

	return GRUB_ERR_NONE;
}

static grub_err_t
grub_TPM_read_tcglog( const unsigned long index ) {

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	grub_uint32_t returnCode, featureFlags, eventLog = 0, logAddr = 0, edi = 0;
	grub_uint8_t major, minor;

	/* get event log pointer */
	grub_err_t err = tcg_statusCheck( &returnCode, &major, &minor, &featureFlags, &eventLog, &edi );

    if( err != GRUB_ERR_NONE ) {
        return err;
    }

	/* edi = 0 means event log is empty */
	if( edi == 0 ) {
        return grub_error (GRUB_ERR_TPM, N_("Event log is empty"));
	}

	logAddr = eventLog;
	TCG_PCClientPCREvent *event;
	/* index = 0: print all entries */
	if ( index == 0 ) {

		/* eventLog = absolute pointer to the beginning of the event log. */
		event = (TCG_PCClientPCREvent *) logAddr;

		/* If there is exactly one entry */
		if( edi == eventLog ) {
			grub_printf( "pcrIndex: %x \n", event->pcrIndex );
			grub_printf( "eventType: %x \n", event->eventType );
			grub_printf( "digest: " );
			print_sha1( event->digest );
			grub_printf( "\n\n" );
		} else {	/* If there is more than one entry */
			do {
				grub_printf( "pcrIndex: %x \n", event->pcrIndex );
				grub_printf( "eventType: %x \n", event->eventType );
				grub_printf( "digest: " );
				print_sha1( event->digest );
				grub_printf( "\n\n" );

				logAddr += TCG_PCR_EVENT_SIZE + event->eventDataSize;
				event = (TCG_PCClientPCREvent *)logAddr;
			} while( logAddr != edi );

			/* print the last one */
			grub_printf( "pcrIndex: %x \n", event->pcrIndex );
			grub_printf( "eventType: %x \n", event->eventType );
			grub_printf( "digest: " );
			print_sha1( event->digest );
			grub_printf( "\n\n" );
		}
	} else { /* print specific entry */
		logAddr = eventLog;

		unsigned long i;
		for( i = 1; i < index; i++ ) {
			event = (TCG_PCClientPCREvent *)logAddr;
			logAddr += TCG_PCR_EVENT_SIZE + event->eventDataSize;

			if( logAddr > edi ) { /* index not valid.  */
                return grub_error (GRUB_ERR_TPM, N_("No entry at specified index"));
			}
		}

		event = (TCG_PCClientPCREvent *)logAddr;
		grub_printf( "pcrIndex: %x \n", event->pcrIndex );
		grub_printf( "eventType: %x \n", event->eventType );
		grub_printf( "digest: " );
		print_sha1( event->digest );
		grub_printf( "\n\n" );
	}

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_tcglog( grub_command_t cmd __attribute__ ((unused)), int argc, char **args) {

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	if ( argc == 0 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "index expected" ) );
	}

	if ( argc > 1 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "Too many arguments" ) );
	}

	unsigned long index = grub_strtoul( args[0], NULL, 10 );

    /* if index is invalid */
    if( grub_errno != GRUB_ERR_NONE ) {
        return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "invalid format for index" ) );
    }

	grub_err_t err = grub_TPM_read_tcglog( index ) ;

    if( err != GRUB_ERR_NONE ) {
        return err;
    }

	return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_measure( grub_command_t cmd __attribute__ ((unused)), int argc, char **args) {

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	if ( argc != 2 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "Wrong number of arguments" ) );
	}

	unsigned long index = grub_strtoul( args[1], NULL, 10 );

    /* if index is invalid */
    if( grub_errno != GRUB_ERR_NONE ) {
        return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "invalid format for index" ) );
    }

	grub_err_t err = grub_TPM_measureFile( args[0], index );

    if( err != GRUB_ERR_NONE ) {
        return err;
	}

  return GRUB_ERR_NONE;
}

/* Invokes assembler function asm_tcg_SetMemoryOverwriteRequestBit()

   Return value = 1 if function successfully completes
   On error see returncode;
   Page 12 TCG Platform Reset Attack Mitigation Specification V 1.0.0
 */
static grub_err_t
tcg_SetMemoryOverwriteRequestBit( const SetMemoryOverwriteRequestBitInputParamBlock* input ) {

    CHECK_FOR_NULL_ARGUMENT( input )

    if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
    }

	/* copy input buffer */
	void* p = grub_map_memory( INPUT_PARAM_BLK_ADDR, input->iPBLength );

	grub_memcpy( p, input, input->iPBLength );

	grub_unmap_memory( p, input->iPBLength );

	SetMemoryOverwriteRequestBitArgs args;
	args.in_ebx = TCPA;
	args.in_ecx = 0;
	args.in_edx = 0;
	args.in_edi = INPUT_PARAM_BLK_ADDR & 0xF;
	args.in_es  = INPUT_PARAM_BLK_ADDR >> 4;

	asm_tcg_SetMemoryOverwriteRequestBit( &args );

	if ( args.out_eax != TCG_PC_OK ) {
        return grub_error( GRUB_ERR_TPM, N_( "tcg_SetMemoryOverwriteRequestBit: asm_tcg_SetMemoryOverwriteRequestBit failed: %x" ), args.out_eax );
	}

	return GRUB_ERR_NONE;
}

/* Sets Memory Overwrite Request bit */
static grub_err_t
grub_TPM_SetMOR_Bit( const unsigned long disableAutoDetect ) {

    if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
    }

	SetMemoryOverwriteRequestBitInputParamBlock input;
	input.iPBLength = 5;
	input.reserved = 0;

	// Reserved disableAutoDetect Reserved MOR-Bit
	// 000             0            000      0

	if( disableAutoDetect ) {
		// disable autodetect
		// 000 1 000 1
		input.memoryOverwriteActionBitValue = 0x11;
	} else{
		// autodetect
		// 000 0 000 1
		input.memoryOverwriteActionBitValue = 0x01;
	}

	grub_err_t err = tcg_SetMemoryOverwriteRequestBit( &input );

    if( err != GRUB_ERR_NONE ) {
        return err;
	}

	return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_setMOR( grub_command_t cmd __attribute__ ((unused)), int argc, char **args) {

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	if ( argc == 0 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "value expected" ) );
	}

	if ( argc > 1 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "Too many arguments" ) );
	}

	unsigned long disableAutoDetect = grub_strtoul( args[0], NULL, 10 );

	/* if disableAutoDetect is invalid */
    if( grub_errno != GRUB_ERR_NONE ) {
        return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "invalid format for 'disableAutoDetect' " ) );
    }

	if( disableAutoDetect > 1 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "Value must be 0 or 1" ) );
	}

	grub_err_t err = grub_TPM_SetMOR_Bit( disableAutoDetect );

    if( err != GRUB_ERR_NONE ) {
        return err;
	}

	return GRUB_ERR_NONE;
}

static grub_err_t
grub_TPM_getRandom( const unsigned long randomBytesRequested, grub_uint8_t* result ) {

	CHECK_FOR_NULL_ARGUMENT( result )
	CHECK_FOR_NULL_ARGUMENT( randomBytesRequested )

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	GetRandomIncoming* getRandomInput;
	PassThroughToTPM_InputParamBlock* passThroughInput;
	grub_uint16_t inputlen = sizeof( *passThroughInput ) - sizeof( passThroughInput->TPMOperandIn ) + sizeof( *getRandomInput );

	/* variable size struct, must be defined here?! */
	/* TPM_GetRandom Outgoing Operand */
	struct {
		grub_uint16_t tag;
		grub_uint32_t paramSize;
		grub_uint32_t returnCode;
		grub_uint32_t randomBytesSize;
		grub_uint8_t randomBytes[randomBytesRequested];
	} __attribute__ ((packed)) *getRandomOutput;

	PassThroughToTPM_OutputParamBlock* passThroughOutput;
	/* FIXME: Why are these additional +47 bytes needed? */
	grub_uint16_t outputlen = sizeof( *passThroughOutput ) - sizeof( passThroughOutput->TPMOperandOut ) + sizeof( *getRandomOutput ) + 47;

	passThroughInput = grub_zalloc( inputlen );
	if( ! passThroughInput ) {
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_getRandom: memory allocation failed" ) );
	}

	passThroughInput->IPBLength = inputlen;
	passThroughInput->OPBLength = outputlen;

	getRandomInput = (void *)passThroughInput->TPMOperandIn;
	getRandomInput->tag = swap16( TPM_TAG_RQU_COMMAND );
	getRandomInput->paramSize = swap32( sizeof( *getRandomInput ) );
	getRandomInput->ordinal = swap32( TPM_ORD_GetRandom );
	getRandomInput->bytesRequested = swap32( (grub_uint32_t) randomBytesRequested );

	passThroughOutput = grub_zalloc( outputlen );
	if( ! passThroughOutput ) {
		grub_free( passThroughInput );
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_getRandom: memory allocation failed" ) );
	}

	grub_uint32_t passThroughTo_TPM_ReturnCode;
	grub_err_t err = tcg_passThroughToTPM( passThroughInput, passThroughOutput, &passThroughTo_TPM_ReturnCode );

    if( err != GRUB_ERR_NONE ) {
		grub_free( passThroughInput );
		grub_free( passThroughOutput );

        return err;
	}

	grub_free( passThroughInput );

	getRandomOutput = (void *)passThroughOutput->TPMOperandOut;
	grub_uint32_t tpm_getRandomReturnCode = swap32( getRandomOutput->returnCode );

	if( tpm_getRandomReturnCode != TPM_SUCCESS ) {
		grub_free( passThroughOutput );

        return grub_error( GRUB_ERR_TPM, N_( "grub_TPM_getRandom: tpm_getRandomReturnCode: %x" ), tpm_getRandomReturnCode );
	}

	if( swap32( getRandomOutput->randomBytesSize ) != randomBytesRequested ) {
		grub_free( passThroughOutput );
		DEBUG_PRINT( ( "tpmOutput->randomBytesSize != randomBytesRequested\n" ) );
		DEBUG_PRINT( ( "tpmOutput->randomBytesSize = %x \n", swap32( getRandomOutput->randomBytesSize ) ) );
		DEBUG_PRINT( ( "randomBytesRequested = %lu \n", randomBytesRequested ) );
        return grub_error( GRUB_ERR_TPM, N_( "grub_TPM_getRandom: tpmOutput->randomBytesSize != randomBytesRequested" ) );
	}

	grub_memcpy( result, getRandomOutput->randomBytes, (grub_uint32_t) randomBytesRequested );

	grub_free( passThroughOutput );
	return GRUB_ERR_NONE;
}

static grub_err_t
grub_TPM_openOIAP_Session( grub_uint32_t* authHandle, grub_uint8_t* nonceEven ) {

    CHECK_FOR_NULL_ARGUMENT( authHandle )
    CHECK_FOR_NULL_ARGUMENT( nonceEven )

    if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	OIAP_Incoming* oiapInput;
	PassThroughToTPM_InputParamBlock* passThroughInput;
	grub_uint16_t inputlen = sizeof( *passThroughInput ) - sizeof( passThroughInput->TPMOperandIn ) + sizeof( *oiapInput );

	OIAP_Outgoing* oiapOutput;
	PassThroughToTPM_OutputParamBlock* passThroughOutput;
	/* FIXME: Why are these additional +47 bytes needed? */
	grub_uint16_t outputlen = sizeof( *passThroughOutput ) - sizeof( passThroughOutput->TPMOperandOut ) + sizeof( *oiapOutput ) + 47 ;

	passThroughInput = grub_zalloc( inputlen );
	if( ! passThroughInput ) {
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_openOIAP_Session: memory allocation failed" ) );
	}

	passThroughInput->IPBLength = inputlen;
	passThroughInput->OPBLength = outputlen;

	oiapInput = (void *)passThroughInput->TPMOperandIn;
	oiapInput->tag = swap16( TPM_TAG_RQU_COMMAND );
	oiapInput->paramSize = swap32( sizeof( *oiapInput ) );
	oiapInput->ordinal = swap32( TPM_ORD_OIAP );

	passThroughOutput = grub_zalloc( outputlen );
	if( ! passThroughOutput ) {
		grub_free( passThroughOutput );
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_openOIAP_Session: memory allocation failed" ) );
	}

	grub_uint32_t passThroughTo_TPM_ReturnCode;
	grub_err_t err = tcg_passThroughToTPM( passThroughInput, passThroughOutput, &passThroughTo_TPM_ReturnCode );

    if( err != GRUB_ERR_NONE ) {
		grub_free( passThroughInput );
		grub_free( passThroughOutput );

        return err;
	}

	grub_free( passThroughInput );

	oiapOutput = (void *)passThroughOutput->TPMOperandOut;
	grub_uint32_t tpm_OIAP_ReturnCode = swap32( oiapOutput->returnCode );

	if( tpm_OIAP_ReturnCode != TPM_SUCCESS ) {
		grub_free( passThroughOutput );

        return grub_error( GRUB_ERR_TPM, N_( "grub_TPM_openOIAP_Session: tpm_OIAP_ReturnCode: %x" ), tpm_OIAP_ReturnCode );
	}

	*authHandle = swap32( oiapOutput->authHandle );

    grub_memcpy( nonceEven, oiapOutput->nonceEven, TPM_NONCE_SIZE );

	grub_free( passThroughOutput );
	return GRUB_ERR_NONE;
}

static grub_err_t
grub_TPM_openOSAP_Session( const grub_uint32_t entityType, const grub_uint16_t entityValue, const grub_uint8_t* nonceOddOSAP,
		grub_uint32_t* authHandle, grub_uint8_t* nonceEven, grub_uint8_t* nonceEvenOSAP ) {

    CHECK_FOR_NULL_ARGUMENT( authHandle )
    CHECK_FOR_NULL_ARGUMENT( nonceEven )
    CHECK_FOR_NULL_ARGUMENT( nonceEvenOSAP )

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	OSAP_Incoming* osapInput;
	PassThroughToTPM_InputParamBlock* passThroughInput;
	grub_uint16_t inputlen = sizeof( *passThroughInput ) - sizeof( passThroughInput->TPMOperandIn ) + sizeof( *osapInput );

	OSAP_Outgoing* osapOutput;
	PassThroughToTPM_OutputParamBlock* passThroughOutput;
	/* FIXME: Why are these additional +47 bytes needed? */
	grub_uint16_t outputlen = sizeof( *passThroughOutput ) - sizeof( passThroughOutput->TPMOperandOut ) + sizeof( *osapOutput ) + 47 ;

	passThroughInput = grub_zalloc( inputlen );
	if( ! passThroughInput ) {
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_openOSAP_Session: memory allocation failed" ) );
	}

	passThroughInput->IPBLength = inputlen;
	passThroughInput->OPBLength = outputlen;

	osapInput = (void *)passThroughInput->TPMOperandIn;
	osapInput->tag = swap16( TPM_TAG_RQU_COMMAND );
	osapInput->paramSize = swap32( sizeof( *osapInput ) );
	osapInput->ordinal = swap32( TPM_ORD_OSAP );
	osapInput->entityType = swap16( entityType );
	osapInput->entityValue = swap32( entityValue );

	grub_memcpy( osapInput->nonceOddOSAP, nonceOddOSAP, TPM_NONCE_SIZE );

	passThroughOutput = grub_zalloc( outputlen );
	if( ! passThroughOutput ) {
		grub_free( passThroughInput );
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_openOSAP_Session: memory allocation failed" ) );
	}

	grub_uint32_t passThroughTo_TPM_ReturnCode;
	grub_err_t err = tcg_passThroughToTPM( passThroughInput, passThroughOutput, &passThroughTo_TPM_ReturnCode );

    if( err != GRUB_ERR_NONE ) {
		grub_free( passThroughInput );
		grub_free( passThroughOutput );

        return err;
	}

	grub_free( passThroughInput );

	osapOutput = (void *)passThroughOutput->TPMOperandOut;
	grub_uint32_t tpm_OSAP_ReturnCode = swap32( osapOutput->returnCode );

	if( tpm_OSAP_ReturnCode != TPM_SUCCESS ) {
		grub_free( passThroughOutput );

        return grub_error( GRUB_ERR_TPM, N_( "grub_TPM_openOSAP_Session: tpm_OSAP_ReturnCode: %x" ), tpm_OSAP_ReturnCode );
	}

	*authHandle = swap32( osapOutput->authHandle );

	grub_memcpy( nonceEven, osapOutput->nonceEven, TPM_NONCE_SIZE );
	grub_memcpy( nonceEvenOSAP, osapOutput->nonceEvenOSAP, TPM_NONCE_SIZE );

	grub_free( passThroughOutput );

	return GRUB_ERR_NONE;
}

/* calculate shared-secret = HMAC( srkAuthData, nonceEvenOSAP || nonceOddOSAP ) */
static grub_err_t
grub_TPM_calculate_osap_sharedSecret( const grub_uint8_t* nonceEvenOSAP, const grub_uint8_t* nonceOddOSAP, grub_uint8_t* result ) {

    CHECK_FOR_NULL_ARGUMENT( nonceEvenOSAP )
	CHECK_FOR_NULL_ARGUMENT( nonceOddOSAP )
	CHECK_FOR_NULL_ARGUMENT( result )

    if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
    }

	grub_size_t dataSize = TPM_NONCE_SIZE * 2;

	grub_uint8_t data[dataSize];
	grub_uint8_t* dataPointer = &data[0];

	grub_memcpy( dataPointer, nonceEvenOSAP, TPM_NONCE_SIZE );

	dataPointer += TPM_NONCE_SIZE;

	grub_memcpy( dataPointer, nonceOddOSAP, TPM_NONCE_SIZE );

	gcry_err_code_t hmacErrorCode = grub_crypto_hmac_buffer( GRUB_MD_SHA1, srkAuthData, SHA1_DIGEST_SIZE, &data[0],
				dataSize, result );

	if( hmacErrorCode ) {
        return grub_error( GRUB_ERR_TPM, N_( "grub_TPM_calculate_osap_sharedSecre failedt: hmacErrorCode: %x" ), hmacErrorCode );
	}

	return GRUB_ERR_NONE;
}

static grub_err_t
grub_TPM_calculate_Auth( const grub_uint8_t* sharedSecret, const grub_uint8_t* digest, const grub_uint8_t* nonceEven, const grub_uint8_t* nonceOdd,
		const grub_uint8_t continueSession, grub_uint8_t* result ) {

	CHECK_FOR_NULL_ARGUMENT( sharedSecret )
	CHECK_FOR_NULL_ARGUMENT( digest )
	CHECK_FOR_NULL_ARGUMENT( nonceEven )
	CHECK_FOR_NULL_ARGUMENT( nonceOdd )
	CHECK_FOR_NULL_ARGUMENT( result )

    if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
    }

	grub_size_t dataSize = SHA1_DIGEST_SIZE /* hashed ordinal and inData */ +
				TPM_NONCE_SIZE /* authLastNonceEven */ +
				TPM_NONCE_SIZE /* nonceOdd */ +
				sizeof( continueSession ) /* continueAuthSession */;

	grub_uint8_t data[dataSize];
	grub_uint8_t* dataPointer = &data[0];

	grub_memcpy( dataPointer, digest, SHA1_DIGEST_SIZE );

	dataPointer += SHA1_DIGEST_SIZE;

	grub_memcpy( dataPointer, nonceEven, TPM_NONCE_SIZE );

	dataPointer += TPM_NONCE_SIZE;

	grub_memcpy( dataPointer, nonceOdd, TPM_NONCE_SIZE );

	dataPointer += TPM_NONCE_SIZE;

	grub_memcpy( dataPointer, &continueSession, sizeof( continueSession ) );

	gcry_err_code_t hmacErrorCode = grub_crypto_hmac_buffer( GRUB_MD_SHA1, sharedSecret, SHA1_DIGEST_SIZE, &data[0],
			dataSize, result );

	if( hmacErrorCode ) {
        return grub_error( GRUB_ERR_TPM, N_( "grub_TPM_calculate_Auth failedt: hmacErrorCode: %x" ), hmacErrorCode );
	}

	return GRUB_ERR_NONE;
}

grub_err_t
grub_TPM_unseal( const grub_uint8_t* sealedBuffer, const grub_size_t inputSize, grub_uint8_t* result, grub_size_t* resultSize ) {

	CHECK_FOR_NULL_ARGUMENT( sealedBuffer )
	CHECK_FOR_NULL_ARGUMENT( resultSize)

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	/* TPM_UNSEAL Incoming Operand */
	struct {
		grub_uint16_t tag;
		grub_uint32_t paramSize;
		grub_uint32_t ordinal;
		grub_uint32_t parentHandle;
		grub_uint8_t  sealedData[inputSize];
		grub_uint32_t authHandle;
		grub_uint8_t  nonceOdd[TPM_NONCE_SIZE];
		grub_uint8_t  continueAuthSession;
		grub_uint8_t  parentAuth[TPM_AUTHDATA_SIZE];
		grub_uint32_t dataAuthHandle;
		grub_uint8_t  dataNonceOdd[TPM_NONCE_SIZE];
		grub_uint8_t  continueDataSession;
		grub_uint8_t  dataAuth[TPM_AUTHDATA_SIZE];
	} __attribute__ ((packed)) *unsealInput;

	PassThroughToTPM_InputParamBlock *passThroughInput;
	grub_uint16_t inputlen = sizeof( *passThroughInput ) - sizeof( passThroughInput->TPMOperandIn ) + sizeof( *unsealInput );

	/* TPM_UNSEAL Outgoing Operand */
	struct {
		grub_uint16_t tag;
		grub_uint32_t paramSize;
		grub_uint32_t returnCode;
		grub_uint32_t secretSize;
		grub_uint8_t  unsealedData[inputSize + 256];		/* FIXME: what size to use here? */
		grub_uint8_t  nonceEven[TPM_NONCE_SIZE];
		grub_uint8_t  continueAuthSession;
		grub_uint8_t  resAuth[TPM_AUTHDATA_SIZE];
		grub_uint8_t  dataNonceEven[TPM_NONCE_SIZE];
		grub_uint8_t  continueDataSession;
		grub_uint8_t  dataAuth[TPM_AUTHDATA_SIZE];
	} __attribute__ ((packed)) *unsealOutput;

	PassThroughToTPM_OutputParamBlock *passThroughOutput;
	/* FIXME: Why are these additional +47 bytes needed? */
	grub_uint16_t outputlen = sizeof( *passThroughOutput ) - sizeof( passThroughOutput->TPMOperandOut ) + sizeof( *unsealOutput ) + 47 ;

	passThroughInput = grub_zalloc( inputlen );
	if( ! passThroughInput ) {
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_unseal: memory allocation failed" ) );
	}

	passThroughInput->IPBLength = inputlen;
	passThroughInput->OPBLength = outputlen;

	unsealInput = (void*) passThroughInput->TPMOperandIn;
	unsealInput->tag = swap16( TPM_TAG_RQU_AUTH2_COMMAND );
	unsealInput->paramSize = swap32( sizeof( *unsealInput ) );
	unsealInput->ordinal = swap32( TPM_ORD_Unseal );
	unsealInput->parentHandle = swap32( TPM_KH_SRK );

	grub_memcpy ( unsealInput->sealedData, sealedBuffer, inputSize );

	/* open OSAP Session */

	/* get random for nonceOddOSAP */
	grub_uint8_t nonceOddOSAP[TPM_NONCE_SIZE];
	grub_err_t err = grub_TPM_getRandom( TPM_NONCE_SIZE, &nonceOddOSAP[0] );

    if( err != GRUB_ERR_NONE ) {
        return err;
	}

	grub_uint32_t authHandle = 0;
	grub_uint8_t authLastNonceEven[TPM_NONCE_SIZE];
	grub_uint8_t nonceEvenOSAP[TPM_NONCE_SIZE];
	err = grub_TPM_openOSAP_Session( TPM_ET_SRK, TPM_KH_SRK, &nonceOddOSAP[0], &authHandle, &authLastNonceEven[0], &nonceEvenOSAP[0] );

    if( err != GRUB_ERR_NONE ) {
        grub_free( passThroughInput );
		return err;
	}

	unsealInput->authHandle = swap32( authHandle );

	grub_uint8_t sharedSecret[SHA1_DIGEST_SIZE];
	err = grub_TPM_calculate_osap_sharedSecret( &nonceEvenOSAP[0], &nonceOddOSAP[0], &sharedSecret[0] );

    if( err != GRUB_ERR_NONE ) {
		grub_free( passThroughInput );
		return err;
	}

	/* open OIAP Session */
	grub_uint8_t dataLastNonceEven[TPM_NONCE_SIZE];
	grub_uint32_t dataAuthHandle = 0;
	err = grub_TPM_openOIAP_Session( &dataAuthHandle, &dataLastNonceEven[0] );

    if( err != GRUB_ERR_NONE ) {
        grub_free( passThroughInput );
		return err;
	}

	unsealInput->dataAuthHandle = swap32( dataAuthHandle );

	/* calc authData */

	/* SHA1( ordinal, inData ) */
	grub_uint32_t dataToHashSize = sizeof( unsealInput->ordinal ) + inputSize;

	grub_uint8_t* dataToHash = grub_zalloc( dataToHashSize );
	if( ! dataToHash ) {
		grub_free( passThroughInput );
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_unseal: memory allocation failed" ) );
	}

	/* copy ordinal */
	grub_memcpy( dataToHash, &unsealInput->ordinal, sizeof( unsealInput->ordinal ) );

	/* copy inData */
	grub_memcpy( dataToHash + sizeof( unsealInput->ordinal ), unsealInput->sealedData, inputSize );

	grub_uint8_t hashResult[SHA1_DIGEST_SIZE];
	grub_crypto_hash( GRUB_MD_SHA1, &hashResult[0], dataToHash, dataToHashSize );
	grub_free( dataToHash );

	/* calc parentAuth */

	/* HMAC( sharedSecret, SHA1( ordinal, inData ) || authLastNonceEven || nonceOdd || continueAuthSession ) */

	/* get random for nonceOdd */
	err = grub_TPM_getRandom( TPM_NONCE_SIZE, unsealInput->nonceOdd );
	if( err != GRUB_ERR_NONE ) {
        grub_free( passThroughInput );
	    return err;
	}

	unsealInput->continueAuthSession = 0;
	err =  grub_TPM_calculate_Auth( &sharedSecret[0], &hashResult[0], &authLastNonceEven[0], unsealInput->nonceOdd, unsealInput->continueAuthSession, unsealInput->parentAuth );

    if ( err != GRUB_ERR_NONE ) {
        grub_free( passThroughInput );
		return err;
	}

	/* calc dataAuth */

	/* HMAC( entity.usageAuth, SHA1( ordinal, inData ) || dataLastNonceEven || dataNonceOdd || continueDataSession ) */

	/* get random for dataNonceOdd */
	err = grub_TPM_getRandom( TPM_NONCE_SIZE, unsealInput->dataNonceOdd );

    if( err != GRUB_ERR_NONE ) {
        grub_free( passThroughInput );
		return err;
	}

	unsealInput->continueDataSession = 0;
	err = grub_TPM_calculate_Auth( blobAuthData, &hashResult[0], &dataLastNonceEven[0], unsealInput->dataNonceOdd, unsealInput->continueDataSession, unsealInput->dataAuth );

    if( err != GRUB_ERR_NONE ) {
        grub_free( passThroughInput );
		return err;
	}

	passThroughOutput = grub_zalloc( outputlen );
	if( ! passThroughOutput ) {
		grub_free( passThroughInput );
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_unseal: memory allocation failed" ) );
	}

	grub_uint32_t passThroughTo_TPM_ReturnCode;
	err = tcg_passThroughToTPM( passThroughInput, passThroughOutput, &passThroughTo_TPM_ReturnCode );

    if( err != GRUB_ERR_NONE ) {
		grub_free( passThroughInput );
		grub_free( passThroughOutput );
        return err;
	}
	grub_free( passThroughInput );

	unsealOutput = (void *)passThroughOutput->TPMOperandOut;
	grub_uint32_t tpm_UnsealReturnCode = swap32( unsealOutput->returnCode );

	if( tpm_UnsealReturnCode != TPM_SUCCESS ) {
		grub_free( passThroughOutput );

		if( tpm_UnsealReturnCode == TPM_AUTHFAIL ) {
            return grub_error( GRUB_ERR_TPM, N_( "grub_TPM_unseal: Authentication failed" ) );
		}

        return grub_error( GRUB_ERR_TPM, N_( "grub_TPM_unseal: Unsealing failed: %x" ), tpm_UnsealReturnCode );
	}

	/* skip check for returned AuthData */

	/* return result */
	*resultSize = swap32( unsealOutput->secretSize );
	result = grub_zalloc( *resultSize );		/* caller has to clean up */

    if( ! result ) {
        grub_free( passThroughOutput );
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_TPM_unseal: memory allocation failed" ) );
    }

	grub_memcpy( result, unsealOutput->unsealedData, *resultSize );

	grub_free( passThroughOutput );
	return GRUB_ERR_NONE;
}

#ifdef TGRUB_DEBUG
static grub_err_t
grub_cmd_unseal( grub_command_t cmd __attribute__ ((unused)), int argc, char **args) {

	if( !grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	if ( argc == 0 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "value expected" ) );
	}

	if ( argc > 1 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "Too many arguments" ) );
	}

	/* open file */
	grub_file_t file = grub_file_open( args[0] );
    if( ! file ) {
        return grub_errno;
    }

	grub_size_t fileSize = file->size;

	DEBUG_PRINT( ( "sealed file size = %d\n", fileSize ) );

	grub_uint8_t* buf = grub_zalloc( fileSize );
	if ( ! buf ) {
		grub_file_close (file);
        return grub_error( GRUB_ERR_OUT_OF_MEMORY, N_( "grub_cmd_unseal: memory allocation failed" ) );
	}

	/* read file */
	if ( grub_file_read( file, buf, fileSize ) != (grub_ssize_t) fileSize ) {
		grub_free( buf );
		grub_file_close (file);
        return grub_errno;
	}

	grub_file_close( file );

    if ( grub_errno ) {
        return grub_errno;
    }

	grub_uint8_t* result = 0;
	grub_size_t resultSize = 0;
	grub_err_t err = grub_TPM_unseal( buf, fileSize, result, &resultSize );

    if( err != GRUB_ERR_NONE ) {
		grub_free( buf );

		if( result ) {
			grub_free( result );
		}

		return err;
	}

	grub_free( buf );
	grub_free( result );

	return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_getRandom( grub_command_t cmd __attribute__ ((unused)), int argc, char **args) {

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	if ( argc == 0 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "value expected" ) );
	}

	if ( argc > 1 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "Too many arguments" ) );
	}

	unsigned long randomBytesRequested = grub_strtoul( args[0], NULL, 10 );

    /* if randomBytesRequested is invalid */
    if( grub_errno != GRUB_ERR_NONE ) {
        return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "invalid format for 'randomBytesRequested' " ) );
    }

	if( randomBytesRequested == 0 ) {
		return grub_error( GRUB_ERR_BAD_ARGUMENT, N_( "Value must be greater 0" ) );
	}

	grub_uint8_t random[randomBytesRequested];

	grub_err_t err = grub_TPM_getRandom( randomBytesRequested, &random[0] );

    if( err != GRUB_ERR_NONE ) {
    	return err;
	}

	unsigned int j;
	for( j = 0; j < randomBytesRequested; ++j ) {
		grub_printf( "%02x", random[j] );
	}

	return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_openOIAP(grub_command_t cmd __attribute__ ((unused)), int argc __attribute__ ((unused)), char** args __attribute__ ((unused))) {

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	grub_uint32_t authHandle = 0;
	grub_uint8_t nonceEven[TPM_NONCE_SIZE];

	grub_err_t err = grub_TPM_openOIAP_Session( &authHandle, &nonceEven[0] );

    if( err != GRUB_ERR_NONE ) {
        return err;
	}

	grub_printf( "authHandle: %x \n", authHandle );

	grub_printf( "nonceEven: " );
	unsigned int j;
	for( j = 0; j < TPM_NONCE_SIZE; ++j ) {
		grub_printf( "%02x", nonceEven[j] );
	}

	return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_openOSAP(grub_command_t cmd __attribute__ ((unused)), int argc __attribute__ ((unused)), char** args __attribute__ ((unused))) {

	if( ! grub_TPM_isAvailable() ) {
        return grub_error (GRUB_ERR_NO_TPM, N_("TPM not available"));
	}

	/* get random for nonceOddOSAP */
	grub_uint8_t nonceOddOSAP[TPM_NONCE_SIZE];
	grub_err_t err = grub_TPM_getRandom( TPM_NONCE_SIZE, &nonceOddOSAP[0] );

    if( err != GRUB_ERR_NONE ) {
        return err;
	}

	grub_uint32_t authHandle = 0;
	grub_uint8_t nonceEven[TPM_NONCE_SIZE];
	grub_uint8_t nonceEvenOSAP[TPM_NONCE_SIZE];

	err = grub_TPM_openOSAP_Session( TPM_ET_SRK, TPM_KH_SRK, &nonceOddOSAP[0], &authHandle, &nonceEven[0], &nonceEvenOSAP[0] );

    if( err != GRUB_ERR_NONE ) {
        return err;
	}

	grub_printf( "authHandle: %x \n", authHandle );

	grub_printf( "nonceEven: " );
	unsigned int j;
	for( j = 0; j < TPM_NONCE_SIZE; ++j ) {
		grub_printf( "%02x", nonceEven[j] );
	}

	grub_printf( "\n nonceEvenOSAP: " );
	for( j = 0; j < TPM_NONCE_SIZE; ++j ) {
		grub_printf( "%02x", nonceEvenOSAP[j] );
	}

	return GRUB_ERR_NONE;
}
#endif

static grub_command_t cmd_readpcr, cmd_tcglog, cmd_measure, cmd_setMOR;

#ifdef TGRUB_DEBUG
	static grub_command_t cmd_random, cmd_oiap, cmd_osap, cmd_unseal;
#endif

GRUB_MOD_INIT(tpm)
{
	cmd_readpcr = grub_register_command( "readpcr", grub_cmd_readpcr, N_( "pcrindex" ),
  		N_( "Display current value of the PCR (Platform Configuration Register) within "
  		    "TPM (Trusted Platform Module) at index, pcrindex." ) );

	cmd_tcglog = grub_register_command( "tcglog", grub_cmd_tcglog, N_( "logindex" ),
		N_( "Displays TCG event log entry at position, logindex. Type in 0 for all entries." ) );

	cmd_measure = grub_register_command( "measure", grub_cmd_measure, N_( "FILE pcrindex" ),
	  	N_( "Perform TCG measurement operation with the file FILE and with PCR( pcrindex )." ) );

	cmd_setMOR = grub_register_command( "setmor", grub_cmd_setMOR, N_( "disableAutoDetect" ),
		  	N_( "Sets Memory Overwrite Request Bit with auto detect enabled (0) or disabled (1)" ) );

#ifdef TGRUB_DEBUG
	cmd_random = grub_register_command( "random", grub_cmd_getRandom, N_( "bytesRequested" ),
			  	N_( "Gets random bytes from TPM." ) );
	cmd_oiap = grub_register_command( "oiap", grub_cmd_openOIAP, 0,
				  	N_( "Opens OIAP Session" ) );
	cmd_osap = grub_register_command( "osap", grub_cmd_openOSAP, 0,
					  	N_( "Opens OSAP Session" ) );
	cmd_unseal = grub_register_command( "unseal", grub_cmd_unseal, N_( "sealedFile" ),
			  	N_( "Unseals 'sealedFile' " ) );
#endif

}

GRUB_MOD_FINI(tpm)
{
	grub_unregister_command( cmd_readpcr );
	grub_unregister_command( cmd_tcglog );
	grub_unregister_command( cmd_measure );
	grub_unregister_command( cmd_setMOR );

#ifdef TGRUB_DEBUG
	grub_unregister_command( cmd_random );
	grub_unregister_command( cmd_oiap );
	grub_unregister_command( cmd_osap );
	grub_unregister_command( cmd_unseal );
#endif

}

/* End TCG extension */
