/*
 * Copyright 2014 Fraunhofer FOKUS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fstream>
#include "rpc_cdm_mediaengine_handler.h"
#include <cdm_logging.h>
#include <opencdm_xdr.h>

#ifdef OCDM_SDP_PROTOTYPE
#include "socket_client_helper.h"
#include "ion_allocator_helper.h"

#ifndef ION_SECURE_HEAP_ID_DECODER
// VPU secure heap ID is required for SDP prototype
#error "ION_SECURE_HEAP_ID_DECODER is not defined"
#endif
#endif

namespace media {

std::string rpcServer = "localhost";

RpcCdmMediaengineHandler& RpcCdmMediaengineHandler::getInstance() {
  static  RpcCdmMediaengineHandler instance;
  return instance;
}

RpcCdmMediaengineHandler::RpcCdmMediaengineHandler() {
  CDM_DLOG() << "RpcCdmMediaengineHandler::RpcCdmMediaengineHandler";

  if ((rpcClient = clnt_create(rpcServer.c_str(), OPEN_CDM, OPEN_CDM_EME_5,
                               "tcp")) == NULL) {
    clnt_pcreateerror(rpcServer.c_str());
    CDM_DLOG() << "rpcclient creation failed: " << rpcServer.c_str();
  } else {
    CDM_DLOG() << "RpcCdmMediaengineHandler connected to server";
  }

 // TODO(ska): need to check == 0?
  // if (idXchngShMem == 0) {
  idXchngShMem = AllocateSharedMemory(sizeof(shmem_info));
  if (idXchngShMem < 0) {
    CDM_DLOG() << "idXchngShMem AllocateSharedMemory failed!";
  }
  // }
  shMemInfo = reinterpret_cast<shmem_info *>(MapSharedMemory(idXchngShMem));

}

bool RpcCdmMediaengineHandler::CreateMediaEngineSession(char *session_id_val,
                                                   uint32_t session_id_len,
                                                   uint8_t *auth_data_val,
                                                   uint32_t auth_data_len) {
  rpc_response_create_mediaengine_session *rpc_response;

  // TODO(ska): do we need this memcpy?

  rpc_request_mediaengine_data rpc_param;
  rpc_param.session_id.session_id_val = session_id_val;
  rpc_param.session_id.session_id_len = session_id_len;
  rpc_param.auth_data.auth_data_val = auth_data_val;
  rpc_param.auth_data.auth_data_len = auth_data_len;

   // SHARED MEMORY and SEMAPHORE INITIALIZATION
  shMemInfo->idSidShMem = 0;
  this->shMemInfo->idIvShMem = 0;
  this->shMemInfo->idSampleShMem = 0;
  this->shMemInfo->idSubsampleDataShMem = 0;
  this->shMemInfo->sidSize = 1;
  this->shMemInfo->ivSize = 2;
  this->shMemInfo->sampleSize = 3;
  this->shMemInfo->subsampleDataSize = 4;

  unsigned short vals[3];
  vals[SEM_XCHNG_PUSH] = 1;
  vals[SEM_XCHNG_DECRYPT] = 0;
  vals[SEM_XCHNG_PULL] = 0;
  // if (idXchngSem == 0) {
  idXchngSem = CreateSemaphoreSet(3, vals);
  if (idXchngSem < 0) {
    CDM_DLOG() << "idXchngSem CreateSemaphoreSet failed!";
  }
  // }

  rpc_param.id_exchange_shmem = idXchngShMem;
  rpc_param.id_exchange_sem = idXchngSem;

  CDM_DLOG() << "Calling rpc_open_cdm_mediaengine_1";
  if ((rpc_response = rpc_open_cdm_mediaengine_1(&rpc_param, rpcClient))
      == NULL) {
    CDM_DLOG() << "engine session failed: " << rpcServer.c_str();
    clnt_perror(rpcClient, rpcServer.c_str());
    return false;
  } else {
    CDM_DLOG() << "engine session creation called";
  }

  CDM_DLOG() << "create media engine session platform response: "
             << rpc_response->platform_val << ", socket channel ID: "
             << rpc_response->socket_channel_id;

#ifdef OCDM_SDP_PROTOTYPE
  /* Connect to the CDMi Service to allow passing the ION file descriptor
     at every decrypt operation. */
  if(this->mSocketClient.Connect(rpc_response->socket_channel_id) < 0) {
    return false;
  }
#endif

  return true;
}

RpcCdmMediaengineHandler::~RpcCdmMediaengineHandler() {
  CDM_DLOG() << "RpcCdmMediaengineHandler destruct!";
  // TODO(ska): is shared memory cleaned up correctly?
  // DeleteSemaphoreSet(idXchngSem);
  delete [] sessionId.id;
  idXchngSem = 0;
  idXchngShMem = 0;
}
int RpcCdmMediaengineHandler::ReleaseMem() {

  shMemInfo->idSidShMem = 0;
  shMemInfo->idIvShMem = 0;
  shMemInfo->idSampleShMem = 0;
  shMemInfo->idSubsampleDataShMem = 0;
  shMemInfo->ivSize = 0;
  shMemInfo->sampleSize = 0;
  UnlockSemaphore(idXchngSem, SEM_XCHNG_DECRYPT);
  LockSemaphore(idXchngSem, SEM_XCHNG_PULL);

  return 1;
}
DecryptResponse RpcCdmMediaengineHandler::Decrypt(const uint8_t *pbIv,
                                                  uint32_t cbIv,
                                                  const uint8_t *pbData,
                                                  uint32_t cbData, uint8_t *out,
                                                  uint32_t &out_size) {
  printf("Decrypt-------\n");
  CDM_DLOG() << "RpcCdmMediaengineHandler::Decrypt: ";
  DecryptResponse response;
  response.platform_response = PLATFORM_CALL_SUCCESS;
  response.sys_err = 0;
#ifdef OCDM_SDP_PROTOTYPE
  int status = 0;
  IonAllocator ionAlloc;
#endif

  // TODO(sph): real decryptresponse values need to
  // be written to sharedmem as well

  out_size = 0;

  LockSemaphore(idXchngSem, SEM_XCHNG_PUSH);
  CDM_DLOG() << "LOCKed push lock";

  cbIv = (cbIv != 8) ? 8 : cbIv;
  shMemInfo->idIvShMem = AllocateSharedMemory(cbIv);
  shMemInfo->ivSize = cbIv;

  uint8_t *pIvShMem = reinterpret_cast<uint8_t *>(MapSharedMemory(
      shMemInfo->idIvShMem));
  memcpy(pIvShMem, pbIv, cbIv);
  // delete[] pbIv;

  shMemInfo->idSampleShMem = AllocateSharedMemory(cbData);
  shMemInfo->sampleSize = cbData;
  uint8_t *pSampleShMem = reinterpret_cast<uint8_t *>(MapSharedMemory(
      shMemInfo->idSampleShMem));

  memcpy(pSampleShMem, pbData, cbData);
  // delete[] pbData;
  CDM_DLOG() << "memcpy pSampleShMem, pbData";

#ifdef OCDM_SDP_PROTOTYPE
  status = ionAlloc.Allocate(cbData, ION_SECURE_HEAP_ID_DECODER);
  if(status < 0) {
    response.platform_response = PLATFORM_CALL_FAIL;
    goto handle_error;
  }

  CDM_DLOG() << "FD: " << ionAlloc.GetFileDescriptor() << ", size: " << ionAlloc.GetSize() << " bytes";

  // Send file FD
  status = this->mSocketClient.SendFileDescriptor(ionAlloc.GetFileDescriptor(), ionAlloc.GetSize());
  if(status < 0) {
    CDM_DLOG() << "Failure to send file descriptor";
    response.platform_response = PLATFORM_CALL_FAIL;
    goto handle_error;
  }
#endif

  shMemInfo->idSubsampleDataShMem = 0;
  shMemInfo->subsampleDataSize = 0;
  CDM_DLOG() << "data ready to decrypt";
  UnlockSemaphore(idXchngSem, SEM_XCHNG_DECRYPT);
  CDM_DLOG() << "WAIT for pull lock";
  LockSemaphore(idXchngSem, SEM_XCHNG_PULL);
  CDM_DLOG() << "LOCKed pull lock";
  // process clear data

#ifndef OCDM_SDP_PROTOTYPE
  memcpy(out, pSampleShMem, cbData);
#else
  CDM_DLOG() << "COPY secure memory to shared memory";
  status = ionAlloc.CopyData(out, cbData);
  if(status < 0) {
    CDM_DLOG() << "Failure to copy secure data to shared memory";
    response.platform_response = PLATFORM_CALL_FAIL;
    goto handle_error;
  }
#endif
  out_size = cbData;

#ifdef OCDM_SDP_PROTOTYPE
handle_error:
  ionAlloc.Free();
#endif

  CDM_DLOG() << "RUN fired!";
  UnlockSemaphore(idXchngSem, SEM_XCHNG_PUSH);
  CDM_DLOG() << "UNLOCKed push lock";

  // clean up current shared mems for sample data
  int err = DetachExistingSharedMemory(pIvShMem);
  CDM_DLOG() << "detached iv shmem " << shMemInfo->idIvShMem << ": " << err;
  err = DetachExistingSharedMemory(pSampleShMem);
  CDM_DLOG() << "detached sample shmem " << shMemInfo->idSampleShMem << ": "
             << err;

  return response;
}


}  // namespace media
