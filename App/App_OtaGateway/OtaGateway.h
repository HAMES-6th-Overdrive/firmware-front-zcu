#ifndef OTA_GATEWAY_H_
#define OTA_GATEWAY_H_

/**********************************************************************************************************************
 * \file OtaGateway.h
 * \brief ZCU OTA Gateway Layer - Download/Verify phase
 *
 * 역할:
 *  - Pi/HPC 계층에서 받은 OTA_START / OTA_BLOCK 요청을 UdsOtaClient streaming API로 연결한다.
 *  - ZCU는 전체 firmware binary를 저장하지 않고, 현재 필요한 62-byte 이하 block만 Sensor ECU로 전달한다.
 *
 * 현재 단계:
 *  - Store 구매 후 bin download
 *  - Sensor ECU inactive slot에 write
 *  - CRC 검증
 *
 * 주의:
 *  - 여기서는 SOTA/UCB_SWAP activation을 수행하지 않는다.
 *  - 사용자가 HPC에서 "업데이트 적용"을 승인하면 별도 Activation routine으로 A/B slot switch를 수행한다.
 *
 * 구조:
 *  Pi/HPC
 *      ↓
 *  App_OtaGateway
 *      ↓
 *  OtaGateway
 *      ↓
 *  UdsOtaClient
 *      ↓
 *  App_Can
 *      ↓
 *  Sensor ECU
 *
 * CRC 모드:
 *  1. 기존 모드
 *     - OtaGateway_Start(firmwareSize, firmwareCrc32)
 *     - OTA 시작 시점에 CRC32를 이미 알고 있다.
 *
 *  2. Late CRC 모드
 *     - OtaGateway_StartWithoutCrc(firmwareSize)
 *     - Pi/HPC -> ZCU DoIP 흐름처럼 CRC32가 마지막 0x37에서 들어오는 경우 사용한다.
 *     - 모든 block 전송 완료 후 WAIT_FINAL_CRC 상태에서 대기한다.
 *     - 이후 OtaGateway_SetFinalCrc(firmwareCrc32)가 호출되면
 *       Sensor ECU 쪽 RequestTransferExit + RoutineControl CRC를 진행한다.
 *********************************************************************************************************************/

#include "Ifx_Types.h"
#include <stdint.h>

/* ============================================================
   Gateway State
   ============================================================ */

typedef enum
{
    OTA_GATEWAY_STATE_IDLE = 0,
    OTA_GATEWAY_STATE_IN_PROGRESS,
    OTA_GATEWAY_STATE_WAIT_BLOCK,

    /*
     * Late CRC mode 전용 상태.
     *
     * 모든 firmware block을 Sensor ECU로 전송한 뒤,
     * Pi/HPC가 0x37 단계에서 CRC32를 줄 때까지 대기한다.
     *
     * OtaGateway_SetFinalCrc()가 호출되면
     * UdsOtaClient가 RequestTransferExit + RoutineControl CRC를 진행한다.
     */
    OTA_GATEWAY_STATE_WAIT_FINAL_CRC,

    OTA_GATEWAY_STATE_DONE,
    OTA_GATEWAY_STATE_ERROR
} OtaGateway_State_t;


/* ============================================================
   Gateway Result
   ============================================================ */

typedef enum
{
    OTA_GATEWAY_RESULT_OK = 0,
    OTA_GATEWAY_RESULT_BUSY,
    OTA_GATEWAY_RESULT_INVALID_PARAM,
    OTA_GATEWAY_RESULT_SEQUENCE_ERROR,
    OTA_GATEWAY_RESULT_CLIENT_ERROR,
    OTA_GATEWAY_RESULT_CANCELLED
} OtaGateway_Result_t;


/* ============================================================
   Debug Info
   ============================================================ */

typedef struct
{
    OtaGateway_State_t  state;
    OtaGateway_Result_t lastResult;

    uint32_t firmwareSize;
    uint32_t firmwareCrc32;

    /*
     * TRUE:
     *  - CRC32를 이미 알고 있는 기존 모드
     *  - 또는 late CRC 모드에서 SetFinalCrc() 호출 완료
     *
     * FALSE:
     *  - StartWithoutCrc()로 시작했고 아직 final CRC를 받지 않은 상태
     */
    boolean finalCrcProvided;

    uint32_t requestedBlockIndex;
    uint32_t requestedOffset;
    uint8_t  requestedLength;

    uint32_t providedBlockCount;
    uint32_t lastProvidedBlockIndex;
    uint32_t lastProvidedOffset;
    uint8_t  lastProvidedLength;

    uint32_t startRequestCount;
    uint32_t startWithoutCrcRequestCount;
    uint32_t finalCrcSetRequestCount;

    uint32_t blockRequestCount;
    uint32_t blockProvideOkCount;
    uint32_t blockProvideFailCount;
    uint32_t cancelRequestCount;

    uint8_t  progressPercent;
} OtaGateway_DebugInfo_t;


/* ============================================================
   Public API
   ============================================================ */

void OtaGateway_Init(void);

void OtaGateway_Reset(void);

/**
 * @brief OTA Download 시작 - CRC known mode
 *
 * Pi/HPC 계층에서 OTA_START(size, crc32)를 받으면 호출한다.
 *
 * 의미:
 *  - 전체 firmware를 ZCU에 저장하지 않는다.
 *  - firmwareSize / firmwareCrc32만 UdsOtaClient에 전달한다.
 *  - 이후 UdsOtaClient가 필요한 block을 요청하면,
 *    Pi/HPC 계층이 OtaGateway_ProvideBlock()으로 해당 block을 제공한다.
 *
 * @param firmwareSize  전체 firmware size
 * @param firmwareCrc32 전체 firmware CRC32
 *
 * @return OTA_GATEWAY_RESULT_OK if accepted
 */
OtaGateway_Result_t OtaGateway_Start(uint32_t firmwareSize,
                                     uint32_t firmwareCrc32);

/**
 * @brief OTA Download 시작 - Late CRC mode
 *
 * Pi/HPC -> ZCU DoIP 흐름에서는 CRC32가 마지막 0x37에서 들어올 수 있다.
 * 이 함수는 firmwareSize만으로 Sensor ECU OTA download를 시작한다.
 *
 * 모든 block 전송 완료 후 Gateway는 OTA_GATEWAY_STATE_WAIT_FINAL_CRC 상태가 된다.
 * 이후 OtaGateway_SetFinalCrc()가 호출되면
 * Sensor ECU 쪽 RequestTransferExit + RoutineControl CRC를 진행한다.
 *
 * @param firmwareSize 전체 firmware size
 *
 * @return OTA_GATEWAY_RESULT_OK if accepted
 */
OtaGateway_Result_t OtaGateway_StartWithoutCrc(uint32_t firmwareSize);

/**
 * @brief Late CRC mode에서 최종 CRC32 설정
 *
 * Pi/HPC -> ZCU DoIP 흐름에서 0x37 RequestTransferExit 단계에 CRC32가 들어오면 호출한다.
 *
 * 호출 조건:
 *  - OtaGateway_IsWaitingFinalCrc() == TRUE
 *
 * @param firmwareCrc32 전체 firmware CRC32
 *
 * @return OTA_GATEWAY_RESULT_OK if accepted
 */
OtaGateway_Result_t OtaGateway_SetFinalCrc(uint32_t firmwareCrc32);

/**
 * @brief 현재 요청된 firmware block 제공
 *
 * Pi/HPC 계층에서 OTA_BLOCK(blockIndex, data, length)를 받으면 호출한다.
 *
 * 호출 조건:
 *  - OtaGateway_IsWaitingBlock() == TRUE
 *  - blockIndex == OtaGateway_GetRequestedBlockIndex()
 *  - length == OtaGateway_GetRequestedLength()
 *
 * @param blockIndex 제공할 block index
 * @param data       block data pointer
 * @param length     block length. 보통 62 bytes, 마지막 block은 62보다 작을 수 있음
 *
 * @return OTA_GATEWAY_RESULT_OK if accepted
 */
OtaGateway_Result_t OtaGateway_ProvideBlock(uint32_t blockIndex,
                                            const uint8_t *data,
                                            uint8_t length);

/**
 * @brief OTA Download 취소
 *
 * 진행 중인 UdsOtaClient 상태를 reset하고 Gateway 상태를 IDLE로 되돌린다.
 *
 * @return OTA_GATEWAY_RESULT_OK
 */
OtaGateway_Result_t OtaGateway_Cancel(void);

/**
 * @brief Gateway 상태 갱신
 *
 * 1ms 주기로 호출 권장.
 *
 * 주의:
 *  - 이 함수 내부에서 UdsOtaClient_MainFunction()도 함께 호출한다.
 *  - 따라서 상위 App/Task는 OtaGateway_MainFunction()만 주기적으로 호출하면 된다.
 */
void OtaGateway_MainFunction(void);

boolean OtaGateway_IsBusy(void);
boolean OtaGateway_IsWaitingBlock(void);
boolean OtaGateway_IsWaitingFinalCrc(void);
boolean OtaGateway_IsDone(void);
boolean OtaGateway_IsError(void);

uint32_t OtaGateway_GetRequestedBlockIndex(void);
uint32_t OtaGateway_GetRequestedOffset(void);
uint8_t  OtaGateway_GetRequestedLength(void);

uint8_t OtaGateway_GetProgress(void);

void OtaGateway_GetDebugInfo(OtaGateway_DebugInfo_t *info);

#endif /* OTA_GATEWAY_H_ */
