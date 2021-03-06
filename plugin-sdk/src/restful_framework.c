/*
 * Copyright (C) 2017 Intel Corporation.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * restful_service_framework.c
 */


#include "plugin_sdk.h"
#include "sdk_internals.h"
#include "plugin_dlist.h"


framework_ctx_t * g_framework_ctx = NULL;
void * init_restful_framework(char * module_name, MODULE_HANDLE module, BROKER_HANDLE broker, bool separate_thread)
{

	if(g_framework_ctx)
		return g_framework_ctx;

	framework_ctx_t * ctx = (framework_ctx_t*) malloc(sizeof (framework_ctx_t));
	if(ctx == NULL)
		return NULL;

	memset(ctx, 0, sizeof(framework_ctx_t));

	ctx->broker_handle = broker;
	ctx->module_handle = module;
	ctx->module_name = strdup(module_name);

	ctx->resources_handlers_lock = Lock_Init();

	ctx->transactions_ctx = create_sync_ctx();

	if(separate_thread)
	{
		ctx->internal_queue = (dlist_entry_ctx_t *) malloc(sizeof(dlist_entry_ctx_t));
		memset(ctx->internal_queue, 0, sizeof(*ctx->internal_queue));
	    DList_InitializeListHead(&(ctx->internal_queue->list_queue));
	    ctx->internal_queue->thread_mutex = Lock_Init();
	}

	g_framework_ctx = ctx;
	return ctx;
}

bool register_resource_handler(const char * url, Plugin_Res_Handler handler, rest_action_t action)
{

	if(action >= MAX_RESTFUL_ACTION) return false;

	Lock(g_framework_ctx->resources_handlers_lock);

	resource_handler_node_t * node = g_framework_ctx->resources_handlers;
	while(node)
	{
		if(strcmp(node->url, url) == 0)
		{
			node->res_handlers[action] = handler;
			break;
		}

		node = node->next;
	}

	if(NULL == node)
	{
		node = (resource_handler_node_t*) malloc(sizeof(resource_handler_node_t));
		memset(node, 0, sizeof(resource_handler_node_t));

		//
		node->url = strdup(url);
		node->res_handlers[action] = handler;

		node->next = g_framework_ctx->resources_handlers;
		g_framework_ctx->resources_handlers = node;
	}


	Unlock(g_framework_ctx->resources_handlers_lock);

	return true;
}

enum
{
	Tag_Request,
	Tag_Response,
	Tag_Evt
};

bool handle_bus_message(MODULE_HANDLE moduleHandle, MESSAGE_HANDLE messageHandle)
{
	bool ret = false;


    CONSTMAP_HANDLE properties = Message_GetProperties(messageHandle); /*by contract this is never NULL*/

    // check if the message is targeted to other modules
    const char* dest = ConstMap_GetValue(properties, XK_DEST);
    if(dest && strcmp(dest, g_framework_ctx->module_name) != 0)
        goto end;

    const char * src = ConstMap_GetValue(properties, XK_SRC);;
    const char* msg_tag = ConstMap_GetValue(properties, XK_TAG);
    if(!msg_tag) goto end;
    int tag;
    if(strcmp(msg_tag,TAG_REST_REQ) == 0)
    {
    	if(src == NULL) goto end;
    	tag = Tag_Request;
    }
    else  if(strcmp(msg_tag,TAG_EVENT) == 0)
    {
    	tag = Tag_Evt;
    }
    else  if(strcmp(msg_tag,TAG_REST_RESP) == 0)
    {
    	// response requires "dest" is set to this module
    	if(dest == NULL) goto end;
    	tag = Tag_Response;
    }
    else
    {
    	goto end;
    }

	const char* sfmt = ConstMap_GetValue(properties, XK_FMT);
	if(sfmt == NULL) goto end;

    uint32_t id = 0;
    const char* mid = ConstMap_GetValue(properties, XK_MID);
    if(mid != NULL)
    	id = (uint32_t) atoi(mid);
    else if(tag != Tag_Evt)
    	goto end;

    const CONSTBUFFER * content = Message_GetContent(messageHandle);

    if( tag == Tag_Evt || tag == Tag_Request)
    {
    	int action = T_Post;
    	const char* msg_action = ConstMap_GetValue(properties, XK_ACTION);
    	if(msg_action != NULL)
    	{
    		action = action_from_string((char*)msg_action);
    		if(action == -1 || action >= MAX_RESTFUL_ACTION)
    		    goto end;
    	}
    	else if(tag == Tag_Request)
    	{
    		goto end;
    	}


    	const char* uri = ConstMap_GetValue(properties, XK_URI);
    	if (uri == NULL) goto end;

    	Lock(g_framework_ctx->resources_handlers_lock);

    	resource_handler_node_t * node = g_framework_ctx->resources_handlers;
    	while(node)
    	{
    		if(node->res_handlers[action] && match_url(node->url, (char*)uri) )
    		{
    			// post it to working thread
    			if(g_framework_ctx->internal_queue)
    			{
    				dlist_post(g_framework_ctx->internal_queue, T_MESSAGE_REQUEST, Message_Clone(messageHandle), node->res_handlers[action]);
    				wakeup_working_thread(g_framework_ctx);
    			}

    			// handle the request here
    			else
    			{
    				restful_request_t request = {0};
    				restful_response_t response = {0};
    				decode_request(messageHandle, &request);

    				Plugin_Res_Handler handler = (Plugin_Res_Handler)node->res_handlers[action];
    				if(handler(&request, &response))
    				{
    					response.mid = request.mid;
    					response.dest_module = (char*)src;

						// todo: send the response to the broker
						MESSAGE_HANDLE response_handle = encode_response(&response);
						if(response_handle)
						{
							(void)Broker_Publish(g_framework_ctx->broker_handle, (MODULE_HANDLE)g_framework_ctx->module_handle, response_handle);
							Message_Destroy(response_handle);
						}
    				}

    				if(response.payload) free(response.payload);
    			}
    			break;
    		}
    		node = node->next;
    	}
    	Unlock(g_framework_ctx->resources_handlers_lock);

        goto end;
    }
    else if(tag == Tag_Response)
    {

       	// trigger the callback function cb_foward_response_to_client()
		bh_feed_response(g_framework_ctx->transactions_ctx, id, messageHandle, 0, T_Broker_Message_Handle);
    }

end:
    ConstMap_Destroy(properties);
}



uint32_t process_in_working_thread(void * ctx)
{
	framework_ctx_t * framework = (framework_ctx_t *) ctx;
	while(framework->internal_queue)
	{
		dlist_node_t *msg =  dlist_get(framework->internal_queue);
		if(msg == NULL)
			break;

		if(msg->type == T_MESSAGE_REQUEST)
		{
			MESSAGE_HANDLE messageHandle = (MESSAGE_HANDLE) msg->message;

			restful_request_t request = {0};
			restful_response_t response = {0};
			decode_request(messageHandle, &request);

			Plugin_Res_Handler handler = (Plugin_Res_Handler) msg->message_handler;
			if(handler(&request, &response))
			{

				response.mid = request.mid;
				response.dest_module = request.src_module;

				// todo: send the response to the broker
				MESSAGE_HANDLE response_handle = encode_response(&response);
				if(response_handle)
				{
					(void)Broker_Publish(framework->broker_handle, (MODULE_HANDLE)framework->module_handle, response_handle);

					Message_Destroy(response_handle);
				}

				Message_Destroy(messageHandle);
			}

			if(response.payload) free(response.payload);
		}

		free(msg);
	}

	return bh_handle_expired_trans(framework->transactions_ctx);
}


