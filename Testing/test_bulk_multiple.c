/*
 * Copyright (C) 2013-2016 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#include "mercury_test.h"
#include "mercury_hl.h"

#ifdef MERCURY_HAS_PARALLEL_TESTING
#include <mpi.h>
#endif

#include <stdio.h>
#include <stdlib.h>

extern hg_id_t hg_test_bulk_write_id_g;

static int my_rank;
static int nranks;

#define MAX_ADDR_NAME 256

struct hg_test_bulk_args {
    hg_handle_t handle;
    size_t nbytes;
    ssize_t ret;
    int fildes;
};

static hg_return_t
hg_test_bulk_forward_cb(const struct hg_cb_info *callback_info)
{
    hg_handle_t handle = callback_info->info.forward.handle;
    hg_request_t *request = (hg_request_t *) callback_info->arg;
    size_t bulk_write_ret = 0;
    bulk_write_out_t bulk_write_out_struct;
    hg_return_t ret = HG_SUCCESS;

    /* Get output */
    ret = HG_Get_output(handle, &bulk_write_out_struct);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "Could not get output\n");
        goto done;
    }

    /* Get output parameters */
    bulk_write_ret = bulk_write_out_struct.ret;
    printf("bulk_write returned: %zu\n", bulk_write_ret);

    /* Free request */
    ret = HG_Free_output(handle, &bulk_write_out_struct);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "Could not free output\n");
        goto done;
    }

    hg_request_complete(request);

done:
    return ret;
}

static hg_return_t
hg_test_bulk_transfer_cb(const struct hg_cb_info *hg_cb_info)
{
    struct hg_test_bulk_args *bulk_args = (struct hg_test_bulk_args *)
            hg_cb_info->arg;
    hg_bulk_t local_bulk_handle = hg_cb_info->info.bulk.local_handle;
    hg_return_t ret = HG_SUCCESS;

    bulk_write_out_t out_struct;

    void *buf;
    size_t write_ret;

    if (hg_cb_info->ret == HG_CANCELED) {
        printf("HG_Bulk_transfer() was successfully canceled\n");

        /* Fill output structure */
        out_struct.ret = 0;
    } else if (hg_cb_info->ret != HG_SUCCESS) {
        HG_LOG_ERROR("Error in callback");
        ret = HG_PROTOCOL_ERROR;
        goto done;
    }

    if (hg_cb_info->ret == HG_SUCCESS) {
        /* Call bulk_write */
        HG_Bulk_access(local_bulk_handle, 0, bulk_args->nbytes,
            HG_BULK_READWRITE, 1, &buf, NULL, NULL);

        write_ret = bulk_args->nbytes;

        /* Fill output structure */
        out_struct.ret = write_ret;
    }

    /* Free block handle */
    ret = HG_Bulk_free(local_bulk_handle);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "Could not free HG bulk handle\n");
        return ret;
    }

    /* Send response back */
    ret = HG_Respond(bulk_args->handle, NULL, NULL, &out_struct);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "Could not respond\n");
        return ret;
    }

done:
    HG_Destroy(bulk_args->handle);
    free(bulk_args);

    return ret;
}

static hg_return_t
hg_test_bulk_write_multiple_cb(hg_handle_t handle)
{
    const struct hg_info *hg_info = NULL;
    hg_bulk_t origin_bulk_handle = HG_BULK_NULL;
    hg_bulk_t local_bulk_handle = HG_BULK_NULL;
    struct hg_test_bulk_args *bulk_args = NULL;
    bulk_write_in_t in_struct;
    hg_return_t ret = HG_SUCCESS;
    int fildes;

    hg_op_id_t hg_bulk_op_id;

    bulk_args = (struct hg_test_bulk_args *) malloc(
            sizeof(struct hg_test_bulk_args));

    /* Keep handle to pass to callback */
    bulk_args->handle = handle;

    /* Get info from handle */
    hg_info = HG_Get_info(handle);

    /* Get input parameters and data */
    ret = HG_Get_input(handle, &in_struct);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "Could not get input\n");
        return ret;
    }

    /* Get parameters */
    fildes = in_struct.fildes;
    origin_bulk_handle = in_struct.bulk_handle;

    bulk_args->nbytes = HG_Bulk_get_size(origin_bulk_handle);
    bulk_args->fildes = fildes;

    /* Create a new block handle to read the data */
    HG_Bulk_create(hg_info->hg_class, 1, NULL, (hg_size_t *) &bulk_args->nbytes,
        HG_BULK_READWRITE, &local_bulk_handle);

    /* Pull bulk data */
    ret = HG_Bulk_transfer(hg_info->context, hg_test_bulk_transfer_cb,
        bulk_args, HG_BULK_PULL, hg_info->addr, origin_bulk_handle, 0,
        local_bulk_handle, 0, bulk_args->nbytes, &hg_bulk_op_id);
    if (ret != HG_SUCCESS) {
        fprintf(stderr, "Could not read bulk data\n");
        return ret;
    }

    /* Test HG_Bulk_Cancel() */
    if (fildes < 0) {
        ret = HG_Bulk_cancel(hg_bulk_op_id);
        if (ret != HG_SUCCESS){
            fprintf(stderr, "Could not cancel bulk data\n");
            return ret;
        }
    }

    HG_Free_input(handle, &in_struct);
    return ret;
}

/******************************************************************************/
int main(int argc, char *argv[])
{
    hg_request_t *request = NULL;
    hg_handle_t handle;
    hg_addr_t addr = HG_ADDR_NULL;
    char addr_str[MAX_ADDR_NAME];
    hg_addr_t my_addr = HG_ADDR_NULL;
    char my_addr_str[MAX_ADDR_NAME];
    size_t addr_str_size = MAX_ADDR_NAME;

    bulk_write_in_t bulk_write_in_struct;

    int fildes = 12345;
    int *bulk_buf = NULL;
    void *buf_ptrs[2];
    size_t buf_sizes[2];
    size_t count =  (1024 * 1024 * MERCURY_TESTING_BUFFER_SIZE) / sizeof(int);
    size_t bulk_size = count * sizeof(int);
    hg_bulk_t bulk_handle = HG_BULK_NULL;

    hg_return_t hg_ret;
    size_t i;
    int dest, source;

    /* Prepare bulk_buf */
    bulk_buf = (int *) malloc(bulk_size);
    for (i = 0; i < count; i++) {
        bulk_buf[i] = (int) i;
    }
    buf_ptrs[0] = bulk_buf;
    buf_sizes[0] = bulk_size;
    buf_ptrs[1] = NULL;
    buf_sizes[1] = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    /* Initialize the interface (for convenience, shipper_test_client_init
     * initializes the network interface with the selected plugin)
     */
    hg_ret = HG_Hl_init("na+sm", HG_TRUE);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not initialize Mercury\n");
        return EXIT_FAILURE;
    }

    HG_Addr_self(HG_CLASS_DEFAULT, &my_addr);
    HG_Addr_to_string(HG_CLASS_DEFAULT, my_addr_str, &addr_str_size, my_addr);

    dest = (my_rank + 1) % nranks;
    source = (my_rank - 1);
    if (source < 0)
        source = nranks - 1;
//    fprintf(stderr, "(%d) Send %s\n", my_rank, my_addr_str);
//    fprintf(stderr, "(%d) Sendrecv from %d to %d\n", my_rank, my_rank, dest);

    MPI_Sendrecv(my_addr_str, MAX_ADDR_NAME, MPI_BYTE, dest, 1,
        addr_str, MAX_ADDR_NAME, MPI_BYTE, source, 1,
        MPI_COMM_WORLD, MPI_STATUS_IGNORE);

//    fprintf(stderr, "(%d) Received %s\n", my_rank, addr_str);

    /* Register test_bulk */
    hg_test_bulk_write_id_g = MERCURY_REGISTER(HG_CLASS_DEFAULT, "hg_test_bulk_write",
            bulk_write_in_t, bulk_write_out_t, hg_test_bulk_write_multiple_cb);

    /* Look up addr using port name info */
    hg_ret = HG_Hl_addr_lookup_wait(HG_CONTEXT_DEFAULT, HG_REQUEST_CLASS_DEFAULT,
        addr_str, &addr, HG_MAX_IDLE_TIME);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not find addr %s\n", addr_str);
        return EXIT_FAILURE;
    }

    request = hg_request_create(HG_REQUEST_CLASS_DEFAULT);

    hg_ret = HG_Create(HG_CONTEXT_DEFAULT, addr, hg_test_bulk_write_id_g, &handle);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not start call\n");
        return EXIT_FAILURE;
    }

    /* Register memory */
    hg_ret = HG_Bulk_create(HG_CLASS_DEFAULT, 2, buf_ptrs, (hg_size_t *) buf_sizes,
            HG_BULK_READ_ONLY, &bulk_handle);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not create bulk data handle\n");
        return EXIT_FAILURE;
    }

    /* Fill input structure */
    bulk_write_in_struct.fildes = fildes;
    bulk_write_in_struct.bulk_handle = bulk_handle;

    /* Forward call to remote addr and get a new request */
    printf("Forwarding bulk_write, op id: %u...\n", hg_test_bulk_write_id_g);
    hg_ret = HG_Forward(handle, hg_test_bulk_forward_cb, request,
            &bulk_write_in_struct);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not forward call\n");
        return EXIT_FAILURE;
    }

    hg_request_wait(request, HG_MAX_IDLE_TIME, NULL);

    /* Free memory handle */
    hg_ret = HG_Bulk_free(bulk_handle);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not free bulk data handle\n");
        return EXIT_FAILURE;
    }

    /* Complete */
    hg_ret = HG_Destroy(handle);
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not complete\n");
        return EXIT_FAILURE;
    }

    hg_request_destroy(request);

    MPI_Barrier(MPI_COMM_WORLD);

    HG_Addr_free(HG_CLASS_DEFAULT, addr);
    HG_Addr_free(HG_CLASS_DEFAULT, my_addr);

    /* Finalize interface */
    hg_ret = HG_Hl_finalize();
    if (hg_ret != HG_SUCCESS) {
        fprintf(stderr, "Could not finalize HG\n");
    }

    MPI_Finalize();

    /* Free bulk data */
    free(bulk_buf);

    return EXIT_SUCCESS;
}
