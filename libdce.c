/*
 * Copyright (c) 2013, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
********************************** Notes ******************************************
*******
********************************* Memory *****************************************
*
*******
********************************* IPC 3.x *****************************************
* Two approaches are followed for IPC MmRpc calls.
* 1. All the parameters which need to be sent and received to/from IPU are coupled in a struct
*     allocated from Shared Memory. Only the adrress of the struct is passed to MmRpc
*     as a pointer argument. This approach is useful as MmRpc in some cases to avoid multiple
*     translations.
*     This approach is followed for :
*     Engine_open(), Engine_close(), create(), control(), delete()
*     For understanding refer to the Mmrpc_test.c in IPC 3.x
* 2. All the parameters which need to be sent are given as separate arguments to
*     MmRpc. This appraoch is needed when you need to translate an address which is
*     ofsetted from a pointer which in itself needs to be translated.
*     This apporach is followed for : process()
*     For understanding, take the example of inbufs argument in process call(). Inbufs
*     allocated in Shared memory and needs to be translated, has the address of Input
*     buffer (allocated from Tiler). It is not possible to give the Input buffer as an argument
*     to Mmrpc for translation until inbufs is given as a parameter to Mmrpc. Therefore inbuf
*     can't be populated inside another Shared memory struct.
* 3. This approach is a workaround to use approach [1] by solving the issue posed by [2].
*     This approach is followed for : get_version()
*     Taking the example of inbufs to explain, the Input buffer address will be one of the
*     parameters of the struct (explained in [1]) along with inbufs address. Therefore the
*     Input buffer address will get translated here. At the IPU, this address needs to be
*     copied back to inbufs.
*********************************************************************************
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

#include <xdc/std.h>

#if defined(BUILDOS_GLP)
#include <xf86drm.h>
#include <omap_drm.h>
#include <omap_dce.h>
#include <omap_drmif.h>
#endif /* BUILDOS_GLP */

/* IPC Headers */
#include <MmRpc.h>

/*DCE Headers */
#include "libdce.h"
#include "dce_rpc.h"
#include "dce_priv.h"
#include "memplugin.h"


#if defined(BUILDOS_GLP)
#ifdef GLP_X11
int dce_auth_x11(int *fd);
#endif /* GLP_X11 */
#ifdef GLP_WAYLAND
int dce_auth_wayland(int *fd);
#endif /* GLP_WAYLAND */

static int                   fd = -1;
static struct omap_device   *dev;
static int                   ioctl_base;
#define CMD(name) (ioctl_base + DRM_OMAP_DCE_##name)

uint32_t    dce_debug = 3;
#endif /* BUILDOS_GLP */


/********************* GLOBALS ***********************/
/* Hande used for Remote Communication                               */
static MmRpc_Handle       MmRpcHandle = NULL;
static pthread_mutex_t    mutex = PTHREAD_MUTEX_INITIALIZER;
static int                count = 0;


/****************** INLINE FUNCTIONS ********************/

static inline void Fill_MmRpc_fxnCtx(MmRpc_FxnCtx *fxnCtx, int fxn_id, int num_params, int num_xlts, MmRpc_Xlt *xltAry)
{
    fxnCtx->fxn_id = fxn_id;
    fxnCtx->num_params = num_params;
    fxnCtx->num_xlts = num_xlts;
    fxnCtx->xltAry = xltAry;
}

static inline void Fill_MmRpc_fxnCtx_Ptr_Params(MmRpc_Param *mmrpc_params, int size, void *addr, void *handle)
{
    mmrpc_params->type = MmRpc_ParamType_Ptr;
    mmrpc_params->param.ptr.size = size;
    mmrpc_params->param.ptr.addr = (size_t)addr;
    mmrpc_params->param.ptr.handle = (size_t)handle;
}

static inline void Fill_MmRpc_fxnCtx_Scalar_Params(MmRpc_Param *mmrpc_params, int size, int data)
{
    mmrpc_params->type = MmRpc_ParamType_Scalar;
    mmrpc_params->param.scalar.size = size;
    mmrpc_params->param.scalar.data = (size_t)data;
}

static inline void Fill_MmRpc_fxnCtx_Xlt_Array(MmRpc_Xlt *mmrpc_xlt, int index, int32_t base, int32_t addr, void *handle)
{
    /* index : index of params filled in FxnCtx                                                                                        */
    /* base  : user Virtual Address as per definition in MmRpc and not base from where offset is calculated */
    /* offset : calculated from address of index                                                                                      */
    mmrpc_xlt->index = index;
    mmrpc_xlt->offset = MmRpc_OFFSET(base, addr);
    mmrpc_xlt->base = *(size_t *)addr; //*((void *)addr);
    mmrpc_xlt->handle = (size_t)handle;
}

/************************ FUNCTIONS **************************/
/* Interface for QNX for parameter buffer allocation                                */
/* These interfaces are implemented to maintain Backward Compatability */
void *dce_alloc(int sz)
{
    return (memplugin_alloc(sz, 0, TILER_1D_BUFFER));
}

void dce_free(void *ptr)
{
    memplugin_free(ptr, TILER_1D_BUFFER);
}

/*************** Startup/Shutdown Functions ***********************/
static int dce_init(void)
{
    dce_error_status    eError = DCE_EOK;
    MmRpc_Params        args;

    printf(" >> dce_init\n");

    pthread_mutex_lock(&mutex);

    count++;
    /* Check if already Initialized */
    _ASSERT(count == 1, DCE_EOK);

    /* Create remote server insance */
    MmRpc_Params_init(&args);

    eError = MmRpc_create(DCE_DEVICE_NAME, &args, &MmRpcHandle);

    _ASSERT_AND_EXECUTE(eError == DCE_EOK, DCE_EIPC_CREATE_FAIL, count--);

    printf("open(/dev/" DCE_DEVICE_NAME ") -> 0x%x\n", (int)MmRpcHandle);
EXIT:
    pthread_mutex_unlock(&mutex);
    return (eError);
}

static void dce_deinit(void)
{
    pthread_mutex_lock(&mutex);

    count--;
    if( count > 0 ) {
        goto EXIT;
    }

    if( MmRpcHandle != NULL ) {
        MmRpc_delete(&MmRpcHandle);
    }
    MmRpcHandle = NULL;
EXIT:
    pthread_mutex_unlock(&mutex);
    return;
}

/*===============================================================*/
/** Engine_open        : Open Codec Engine.
 *
 * @ param attrs  [in]       : Engine Attributes. This param is not passed to Remote core.
 * @ param name [in]       : Name of Encoder or Decoder codec.
 * @ param ec [out]         : Error returned by Codec Engine.
 * @ return : Codec Engine Handle is returned to be used to create codec.
 *                 In case of error, NULL is returned as Engine Handle.
 */
Engine_Handle Engine_open(String name, Engine_Attrs *attrs, Engine_Error *ec)
{
    MmRpc_FxnCtx        fxnCtx;
    int32_t             fxnRet;
    dce_error_status    eError = DCE_EOK;
    dce_engine_open    *engine_open_msg = NULL;
    Engine_Handle       eng_handle = NULL;

    _ASSERT(name != '\0', DCE_EINVALID_INPUT);

    /* Initialize DCE and IPC. In case of Error Deinitialize them */
    _ASSERT_AND_EXECUTE(dce_init() == DCE_EOK, DCE_EIPC_CREATE_FAIL, dce_deinit());

    printf(">> Engine_open Params::name = %s size = %d\n", name, strlen(name));
    /* Allocate Shared memory for the engine_open rpc msg structure*/
    /* Tiler Memory preferred for now for First level testing */
    engine_open_msg = memplugin_alloc(sizeof(dce_engine_open), 0, TILER_1D_BUFFER);

    _ASSERT_AND_EXECUTE(engine_open_msg != NULL, DCE_EOUT_OF_MEMORY, eng_handle = NULL);

    /* Populating the msg structure with all the params */
    /* Populating all params into a struct avoid individual address translations of name, ec */
    strncpy(engine_open_msg->name, name, strlen(name));
    engine_open_msg->eng_handle = NULL;

    /* Marshall function arguments into the send buffer */
    Fill_MmRpc_fxnCtx(&fxnCtx, DCE_RPC_ENGINE_OPEN, 1, 0, NULL);
    Fill_MmRpc_fxnCtx_Ptr_Params(fxnCtx.params, sizeof(dce_engine_open), engine_open_msg, NULL);

    /* Invoke the Remote function through MmRpc */
    eError = MmRpc_call(MmRpcHandle, &fxnCtx, &fxnRet);

    /* In case of Error, the Application will get a NULL Engine Handle */
    _ASSERT_AND_EXECUTE(eError == DCE_EOK, DCE_EIPC_CALL_FAIL, eng_handle = NULL);

    /* Populate return arguments */
    eng_handle = engine_open_msg->eng_handle;
    ec[0] = engine_open_msg->error_code;

EXIT:
    memplugin_free(engine_open_msg, TILER_1D_BUFFER);

    return (eng_handle);
}

/*===============================================================*/
/** Engine_close           : Close Engine.
 *
 * @ param engine  [in]    : Engine Handle obtained in Engine_open() call.
 */
Void Engine_close(Engine_Handle engine)
{
    MmRpc_FxnCtx        fxnCtx;
    int32_t             fxnRet;
    dce_error_status    eError = DCE_EOK;
    dce_engine_close   *engine_close_msg = NULL;

    _ASSERT(engine != NULL, DCE_EINVALID_INPUT);

    /* Allocate Shared/Tiler memory for the engine_close rpc msg structure*/
    engine_close_msg = memplugin_alloc(sizeof(dce_engine_close), 0, TILER_1D_BUFFER);

    _ASSERT(engine_close_msg != NULL, DCE_EOUT_OF_MEMORY);

    /* Populating the msg structure with all the params */
    engine_close_msg->eng_handle = engine;

    /* Marshall function arguments into the send buffer */
    Fill_MmRpc_fxnCtx(&fxnCtx, DCE_RPC_ENGINE_CLOSE, 1, 0, NULL);
    Fill_MmRpc_fxnCtx_Ptr_Params(fxnCtx.params, sizeof(dce_engine_close), engine_close_msg, NULL);

    /* Invoke the Remote function through MmRpc */
    eError = MmRpc_call(MmRpcHandle, &fxnCtx, &fxnRet);

    _ASSERT(eError == DCE_EOK, DCE_EIPC_CALL_FAIL);

EXIT:
    memplugin_free(engine_close_msg, TILER_1D_BUFFER);

    dce_deinit();
    return;
}

/*===============================================================*/
/** Functions create(), control(), get_version(), process(), delete() are common codec
 * glue function signatures which are same for both encoder and decoder
 */
/*===============================================================*/
/** create         : Create Encoder/Decoder codec.
 *
 * @ param engine  [in]    : Engine Handle obtained in Engine_open() call.
 * @ param name [in]       : Name of Encoder or Decoder codec.
 * @ param params [in]     : Static parameters of codec.
 * @ param codec_id [in]  : To differentiate between Encoder and Decoder codecs.
 * @ return : Codec Handle is returned to be used for control, process, delete calls.
 *                 In case of error, NULL is returned.
 */
static void *create(Engine_Handle engine, String name, void *params, dce_codec_type codec_id)
{
    MmRpc_FxnCtx        fxnCtx;
    MmRpc_Xlt           xltAry;
    int32_t             fxnRet;
    dce_error_status    eError = DCE_EOK;
    dce_codec_create   *codec_create_msg = NULL;
    void               *codec_handle = NULL;

    _ASSERT(name != '\0', DCE_EINVALID_INPUT);
    _ASSERT(engine != NULL, DCE_EINVALID_INPUT);
    _ASSERT(params != NULL, DCE_EINVALID_INPUT);

    /* Allocate Shared/Tiler memory for the codec_create rpc msg structure*/
    codec_create_msg = memplugin_alloc(sizeof(dce_codec_create), 0, TILER_1D_BUFFER);

    _ASSERT_AND_EXECUTE(codec_create_msg != NULL, DCE_EOUT_OF_MEMORY, codec_handle = NULL);

    /* Populating the msg structure with all the params */
    codec_create_msg->engine = engine;
    strncpy(codec_create_msg->codec_name, name, strlen(name));
    codec_create_msg->codec_id = codec_id;
    codec_create_msg->codec_handle = NULL;
    codec_create_msg->static_params = params;

    /* Marshall function arguments into the send buffer */
    Fill_MmRpc_fxnCtx(&fxnCtx, DCE_RPC_CODEC_CREATE, 1, 1, &xltAry);
    Fill_MmRpc_fxnCtx_Ptr_Params(&(fxnCtx.params[0]), sizeof(dce_codec_create), codec_create_msg, NULL);

    /* Mention the virtual pointers that need translation */
    /* Allocations through dce_alloc need translation     */
    /* In this case the static params buffer need translation */
    Fill_MmRpc_fxnCtx_Xlt_Array(fxnCtx.xltAry, 0, (int32_t)codec_create_msg, (int32_t)&(codec_create_msg->static_params), NULL);

    /* Invoke the Remote function through MmRpc */
    eError = MmRpc_call(MmRpcHandle, &fxnCtx, &fxnRet);

    /* In case of Error, the Application will get a NULL Codec Handle */
    _ASSERT_AND_EXECUTE(eError == DCE_EOK, DCE_EIPC_CALL_FAIL, codec_handle = NULL);

    codec_handle = codec_create_msg->codec_handle;

EXIT:
    memplugin_free(codec_create_msg, TILER_1D_BUFFER);

    return (codec_handle);
}

/*===============================================================*/
/** control               : Codec control call.
 *
 * @ param codec  [in]     : Codec Handle obtained in create() call.
 * @ param id [in]            : Command id for XDM control operation.
 * @ param dynParams [in] : Dynamic input parameters to Codec.
 * @ param status [out]    : Codec returned status parameters.
 * @ param codec_id [in]  : To differentiate between Encoder and Decoder codecs.
 * @ return : Status of control() call is returned.
 *                #XDM_EOK                  [0]   :  Success.
 *                #XDM_EFAIL                [-1] :  Failure.
 *                #IPC_FAIL                   [-2] : MmRpc Call failed.
 *                #XDM_EUNSUPPORTED [-3] :  Unsupported request.
 *                #OUT_OF_MEMORY       [-4] :  Out of Shared Memory.
 */
static XDAS_Int32 control(void *codec, int id, void *dynParams, void *status, dce_codec_type codec_id)
{
    MmRpc_FxnCtx         fxnCtx;
    MmRpc_Xlt            xltAry[2];
    int32_t              fxnRet;
    dce_error_status     eError = DCE_EOK;
    dce_codec_control   *codec_control_msg = NULL;

    _ASSERT(codec != NULL, DCE_EINVALID_INPUT);
    _ASSERT(dynParams != NULL, DCE_EINVALID_INPUT);
    _ASSERT(status != NULL, DCE_EINVALID_INPUT);

    /* Allocate Shared/Tiler memory for the codec_control rpc msg structure*/
    codec_control_msg = memplugin_alloc(sizeof(dce_codec_control), 0, TILER_1D_BUFFER);

    _ASSERT(codec_control_msg != NULL, DCE_EOUT_OF_MEMORY);

    /* Populating the msg structure with all the params */
    codec_control_msg->codec_handle = codec;
    codec_control_msg->cmd_id = id;
    codec_control_msg->codec_id = codec_id;
    codec_control_msg->dyn_params = dynParams;
    codec_control_msg->status = status;

    /* Marshall function arguments into the send buffer */
    Fill_MmRpc_fxnCtx(&fxnCtx, DCE_RPC_CODEC_CONTROL, 1, 2, xltAry);
    Fill_MmRpc_fxnCtx_Ptr_Params(fxnCtx.params, sizeof(dce_codec_control), codec_control_msg, NULL);

    /* Dynamic and status params buffer need translation */
    Fill_MmRpc_fxnCtx_Xlt_Array(fxnCtx.xltAry, 0, (int32_t)codec_control_msg, (int32_t)&(codec_control_msg->dyn_params), NULL);
    Fill_MmRpc_fxnCtx_Xlt_Array(&(fxnCtx.xltAry[1]), 0, (int32_t)codec_control_msg, (int32_t)&(codec_control_msg->status), NULL);

    /* Invoke the Remote function through MmRpc */
    eError = MmRpc_call(MmRpcHandle, &fxnCtx, &fxnRet);

    _ASSERT(eError == DCE_EOK, DCE_EIPC_CALL_FAIL);

    eError = codec_control_msg->result;

EXIT:
    memplugin_free(codec_control_msg, TILER_1D_BUFFER);

    return (eError);
}

/*===============================================================*/
/** get_version        : Codec control call to get the codec version. This call has been made
 *                                     separate from control call because it involves an additional version
 *                                     buffer translation.
 *
 * @ param codec  [in]     : Codec Handle obtained in create() call.
 * @ param id [in]            : Command id for XDM control operation.
 * @ param dynParams [in] : Dynamic input parameters to Codec.
 * @ param status [out]    : Codec returned status parameters.
 * @ param codec_id [in]  : To differentiate between Encoder and Decoder codecs.
 * @ return : Status of control() call is returned.
 *                #XDM_EOK                  [0]   :  Success.
 *                #XDM_EFAIL                [-1] :  Failure.
 *                #IPC_FAIL                   [-2] : MmRpc Call failed.
 *                #XDM_EUNSUPPORTED [-3] :  Unsupported request.
 *                #OUT_OF_MEMORY       [-4] :  Out of Shared Memory.
 */
static XDAS_Int32 get_version(void *codec, void *dynParams, void *status, dce_codec_type codec_id)
{
    MmRpc_FxnCtx             fxnCtx;
    MmRpc_Xlt                xltAry[3];
    int32_t                  fxnRet;
    dce_error_status         eError = DCE_EOK;
    dce_codec_get_version   *codec_get_version_msg = NULL;

    _ASSERT(codec != NULL, DCE_EINVALID_INPUT);
    _ASSERT(dynParams != NULL, DCE_EINVALID_INPUT);
    _ASSERT(status != NULL, DCE_EINVALID_INPUT);

    /* Allocate Shared/Tiler memory for the codec_get_version rpc msg structure*/
    codec_get_version_msg = memplugin_alloc(sizeof(dce_codec_get_version), 0, TILER_1D_BUFFER);

    _ASSERT(codec_get_version_msg != NULL, DCE_EOUT_OF_MEMORY);

    /* Populating the msg structure with all the params */
    codec_get_version_msg->codec_handle = codec;
    codec_get_version_msg->codec_id = codec_id;
    codec_get_version_msg->dyn_params = dynParams;
    codec_get_version_msg->status = status;
    if( codec_id == OMAP_DCE_VIDDEC3 ) {
        codec_get_version_msg->version = ((IVIDDEC3_Status *)status)->data.buf;
    } else if( codec_id == OMAP_DCE_VIDENC2 ) {
        codec_get_version_msg->version = ((IVIDDEC3_Status *)status)->data.buf;
    }

    /* Marshall function arguments into the send buffer */
    Fill_MmRpc_fxnCtx(&fxnCtx, DCE_RPC_CODEC_CONTROL, 1, 3, xltAry);
    Fill_MmRpc_fxnCtx_Ptr_Params(fxnCtx.params, sizeof(dce_codec_get_version), codec_get_version_msg, NULL);

    /* Dynamic, status params and version info buffer need translation */
    Fill_MmRpc_fxnCtx_Xlt_Array(fxnCtx.xltAry, 0, (int32_t)codec_get_version_msg, (int32_t)&(codec_get_version_msg->dyn_params), NULL);
    Fill_MmRpc_fxnCtx_Xlt_Array(&(fxnCtx.xltAry[1]), 0, (int32_t)codec_get_version_msg, (int32_t)&(codec_get_version_msg->status), NULL);
    Fill_MmRpc_fxnCtx_Xlt_Array(&(fxnCtx.xltAry[2]), 0, (int32_t)codec_get_version_msg, (int32_t)&(codec_get_version_msg->version), NULL);

    /* Invoke the Remote function through MmRpc */
    eError = MmRpc_call(MmRpcHandle, &fxnCtx, &fxnRet);

    _ASSERT(eError == DCE_EOK, DCE_EIPC_CALL_FAIL);

    eError = codec_get_version_msg->result;

EXIT:
    memplugin_free(codec_get_version_msg, TILER_1D_BUFFER);

    return (eError);
}

typedef enum process_call_params {
    CODEC_HANDLE_INDEX = 0,
    INBUFS_INDEX,
    OUTBUFS_INDEX,
    INARGS_INDEX,
    OUTARGS_INDEX,
    CODEC_ID_INDEX
} process_call_params;

/*===============================================================*/
/** process               : Encode/Decode process.
 *
 * @ param codec  [in]     : Codec Handle obtained in create() call.
 * @ param inBufs [in]     : Input buffer details.
 * @ param outBufs [in]    : Output buffer details.
 * @ param inArgs [in]     : Input arguments.
 * @ param outArgs [out]  : Output arguments.
 * @ param codec_id [in]  : To differentiate between Encoder and Decoder codecs.
 * @ return : Status of the process call.
 *                #XDM_EOK                  [0]   :  Success.
 *                #XDM_EFAIL                [-1] :  Failure.
 *                #IPC_FAIL                   [-2] :  MmRpc Call failed.
 *                #XDM_EUNSUPPORTED [-3] :  Unsupported request.
 */
static XDAS_Int32 process(void *codec, void *inBufs, void *outBufs,
                          void *inArgs, void *outArgs, dce_codec_type codec_id)
{
    MmRpc_FxnCtx        fxnCtx;
    MmRpc_Xlt           xltAry[MAX_TOTAl_BUF];
    int                 fxnRet, count, total_count, numInBufs = 0, numOutBufs = 0, sz[5] = { 0 };
    dce_error_status    eError = DCE_EOK;

    _ASSERT(codec != NULL, DCE_EINVALID_INPUT);
    _ASSERT(inBufs != NULL, DCE_EINVALID_INPUT);
    _ASSERT(outBufs != NULL, DCE_EINVALID_INPUT);
    _ASSERT(inArgs != NULL, DCE_EINVALID_INPUT);
    _ASSERT(outArgs != NULL, DCE_EINVALID_INPUT);

    if( codec_id == OMAP_DCE_VIDDEC3 ) {
        numInBufs = ((XDM2_BufDesc *)inBufs)->numBufs;
        numOutBufs = ((XDM2_BufDesc *)outBufs)->numBufs;
        sz[INBUFS_INDEX] = sizeof(XDM2_BufDesc);
        sz[OUTBUFS_INDEX] = sizeof(XDM2_BufDesc);
        sz[INARGS_INDEX] = sizeof(VIDDEC3_InArgs);
        sz[OUTARGS_INDEX] = sizeof(VIDDEC3_OutArgs);
    } else if( codec_id == OMAP_DCE_VIDENC2 ) {
        numInBufs = ((IVIDEO2_BufDesc *)inBufs)->numPlanes;
        numOutBufs = ((XDM2_BufDesc *)outBufs)->numBufs;
        sz[INBUFS_INDEX] = sizeof(IVIDEO2_BufDesc);
        sz[OUTBUFS_INDEX] = sizeof(XDM2_BufDesc);
        sz[INARGS_INDEX] = sizeof(VIDENC2_InArgs);
        sz[OUTARGS_INDEX] = sizeof(VIDENC2_OutArgs);
    }

    /* marshall function arguments into the send buffer                       */
    /* Approach [2] as explained in "Notes" used for process               */
    Fill_MmRpc_fxnCtx(&fxnCtx, DCE_RPC_CODEC_PROCESS, 6, numInBufs + numOutBufs, xltAry);
    Fill_MmRpc_fxnCtx_Scalar_Params(&(fxnCtx.params[CODEC_HANDLE_INDEX]), sizeof(int32_t), (int32_t)codec);
    Fill_MmRpc_fxnCtx_Ptr_Params(&(fxnCtx.params[INBUFS_INDEX]), sz[INBUFS_INDEX], inBufs, NULL);
    Fill_MmRpc_fxnCtx_Ptr_Params(&(fxnCtx.params[OUTBUFS_INDEX]), sz[OUTBUFS_INDEX], outBufs, NULL);
    Fill_MmRpc_fxnCtx_Ptr_Params(&(fxnCtx.params[INARGS_INDEX]), sz[INARGS_INDEX], inArgs, NULL);
    Fill_MmRpc_fxnCtx_Ptr_Params(&(fxnCtx.params[OUTARGS_INDEX]), sz[OUTARGS_INDEX], outArgs, NULL);
    Fill_MmRpc_fxnCtx_Scalar_Params(&(fxnCtx.params[CODEC_ID_INDEX]), sizeof(int32_t), codec_id);

    /* InBufs, OutBufs, InArgs, OutArgs buffer need translation but since they have been */
    /* individually mentioned as fxnCtx Params, they need not be mentioned below again */
    /* Input and Output Buffers have to be mentioned for translation                               */
    for( count = 0, total_count = 0; count < numInBufs; count++, total_count++ ) {
        if( codec_id == OMAP_DCE_VIDDEC3 ) {
            Fill_MmRpc_fxnCtx_Xlt_Array(&(fxnCtx.xltAry[total_count]), INBUFS_INDEX, (int32_t)inBufs, (int32_t)&(((XDM2_BufDesc *)inBufs)->descs[count].buf), NULL);
        } else if( codec_id == OMAP_DCE_VIDENC2 ) {
            Fill_MmRpc_fxnCtx_Xlt_Array(&(fxnCtx.xltAry[total_count]), INBUFS_INDEX, (int32_t)inBufs, (int32_t)&(((IVIDEO2_BufDesc *)inBufs)->planeDesc[count].buf), NULL);
        }
    }

    for( count = 0; count < numOutBufs; count++, total_count++ ) {
        Fill_MmRpc_fxnCtx_Xlt_Array(&(fxnCtx.xltAry[total_count]), OUTBUFS_INDEX, (int32_t)outBufs, (int32_t)&(((XDM2_BufDesc *)outBufs)->descs[count].buf), NULL);
    }

    /* Invoke the Remote function through MmRpc */
    eError = MmRpc_call(MmRpcHandle, &fxnCtx, &fxnRet);

    _ASSERT(eError == DCE_EOK, DCE_EIPC_CALL_FAIL);

    eError = (dce_error_status)(fxnRet);
EXIT:
    return (eError);
}

/*===============================================================*/
/** delete                : Delete Encode/Decode codec instance.
 *
 * @ param codec  [in]     : Codec Handle obtained in create() call.
 * @ param codec_id [in]  : To differentiate between Encoder and Decoder codecs.
 * @ return : NIL.
 */
static void delete(void *codec, dce_codec_type codec_id)
{
    MmRpc_FxnCtx        fxnCtx;
    int32_t             fxnRet;
    dce_error_status    eError = DCE_EOK;
    dce_codec_delete   *codec_delete_msg = NULL;

    _ASSERT(codec != NULL, DCE_EINVALID_INPUT);

    /* Allocate Shared/Tiler memory for the codec_delete rpc msg structure*/
    codec_delete_msg = memplugin_alloc(sizeof(dce_codec_delete), 0, TILER_1D_BUFFER);

    _ASSERT(codec_delete_msg != NULL, DCE_EOUT_OF_MEMORY);

    /* Populating the msg structure with all the params */
    codec_delete_msg->codec_handle = codec;
    codec_delete_msg->codec_id = codec_id;

    /* Marshall function arguments into the send buffer */
    Fill_MmRpc_fxnCtx(&fxnCtx, DCE_RPC_CODEC_DELETE, 1, 0, NULL);
    Fill_MmRpc_fxnCtx_Ptr_Params(fxnCtx.params, sizeof(dce_codec_delete), codec_delete_msg, NULL);

    /* Invoke the Remote function through MmRpc */
    eError = MmRpc_call(MmRpcHandle, &fxnCtx, &fxnRet);

    _ASSERT(eError == DCE_EOK, DCE_EIPC_CALL_FAIL);

EXIT:
    memplugin_free(codec_delete_msg, TILER_1D_BUFFER);

    return;
}

/*************** Deocder Codec Engine Functions ***********************/
VIDDEC3_Handle VIDDEC3_create(Engine_Handle engine, String name,
                              VIDDEC3_Params *params)
{
    VIDDEC3_Handle    codec;

    DEBUG(">> engine=%p, name=%s, params=%p", engine, name, params);
    codec = create(engine, name, params, OMAP_DCE_VIDDEC3);
    DEBUG("<< codec=%p", codec);
    return (codec);
}

XDAS_Int32 VIDDEC3_control(VIDDEC3_Handle codec, VIDDEC3_Cmd id,
                           VIDDEC3_DynamicParams *dynParams, VIDDEC3_Status *status)
{
    XDAS_Int32    ret;

    DEBUG(">> codec=%p, id=%d, dynParams=%p, status=%p",
          codec, id, dynParams, status);
    if( id == XDM_GETVERSION ) {
        ret = get_version(codec, dynParams, status, OMAP_DCE_VIDDEC3);
    } else {
        ret = control(codec, id, dynParams, status, OMAP_DCE_VIDDEC3);
    }
    DEBUG("<< ret=%d", ret);
    return (ret);
}

XDAS_Int32 VIDDEC3_process(VIDDEC3_Handle codec,
                           XDM2_BufDesc *inBufs, XDM2_BufDesc *outBufs,
                           VIDDEC3_InArgs *inArgs, VIDDEC3_OutArgs *outArgs)
{
    XDAS_Int32    ret;

    DEBUG(">> codec=%p, inBufs=%p, outBufs=%p, inArgs=%p, outArgs=%p",
          codec, inBufs, outBufs, inArgs, outArgs);
    ret = process(codec, inBufs, outBufs, inArgs, outArgs, OMAP_DCE_VIDDEC3);
    DEBUG("<< ret=%d", ret);
    return (ret);
}

Void VIDDEC3_delete(VIDDEC3_Handle codec)
{
    DEBUG(">> codec=%p", codec);
    delete(codec, OMAP_DCE_VIDDEC3);
    DEBUG("<<");
}

/*************** Enocder Codec Engine Functions ***********************/
VIDENC2_Handle VIDENC2_create(Engine_Handle engine, String name,
                              VIDENC2_Params *params)
{
    VIDENC2_Handle    codec;

    DEBUG(">> engine=%p, name=%s, params=%p", engine, name, params);
    codec = create(engine, name, params, OMAP_DCE_VIDENC2);
    DEBUG("<< codec=%p", codec);
    return (codec);
}

XDAS_Int32 VIDENC2_control(VIDENC2_Handle codec, VIDENC2_Cmd id,
                           VIDENC2_DynamicParams *dynParams, VIDENC2_Status *status)
{
    XDAS_Int32    ret;

    DEBUG(">> codec=%p, id=%d, dynParams=%p, status=%p",
          codec, id, dynParams, status);
    ret = control(codec, id, dynParams, status, OMAP_DCE_VIDENC2);
    DEBUG("<< ret=%d", ret);
    return (ret);
}

XDAS_Int32 VIDENC2_process(VIDENC2_Handle codec,
                           IVIDEO2_BufDesc *inBufs, XDM2_BufDesc *outBufs,
                           VIDENC2_InArgs *inArgs, VIDENC2_OutArgs *outArgs)
{
    XDAS_Int32    ret;

    DEBUG(">> codec=%p, inBufs=%p, outBufs=%p, inArgs=%p, outArgs=%p",
          codec, inBufs, outBufs, inArgs, outArgs);
    ret = process(codec, inBufs, outBufs, inArgs, outArgs, OMAP_DCE_VIDENC2);
    DEBUG("<< ret=%d", ret);
    return (ret);
}

Void VIDENC2_delete(VIDENC2_Handle codec)
{
    DEBUG(">> codec=%p", codec);
    delete(codec, OMAP_DCE_VIDENC2);
    DEBUG("<<");
}

