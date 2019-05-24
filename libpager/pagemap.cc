/* -*- mode: C++; indent-tabs-mode: nil -*-

   libpager pagemap code

   Copyright (C) 2017 Brent Baccala <cosine@freesoft.org>

   GNU General Public License version 2 or later (your option)

   written in C++11 (uses move semantics)

   TODO:
   - pagemap[] is statically allocated
   - update NOTES to reflect code factorization
   - handle multi page operations
*/

#include <cassert>
#include <iostream>
#include <algorithm>

#include "pager.h"
#include "pagemap.h"
#include "machMessage.h"

extern "C" {
#include <mach/mach_interface.h>

// this is here for a single call to m_o_change_completed in object_terminate()
#include <mach/memory_object_user.h>
}

// 1. create an empty pagemap entry
// 2. initialize it into pagemap_set (which copies it)
// 3. extract a pointer to the copy inside pagemap_set

// pagemap_entry::data empty_page;
// std::set<pagemap_entry::data> pagemap_entry::pagemap_set {empty_page};

// 1. create pagemap_set and initialize it with a single empty pagemap entry
// 2. extract a pointer to that entry and save it as empty_page_ptr

std::set<pagemap_entry::data> pagemap_set {pagemap_entry::data()};
std::mutex pagemap_set_mutex;
const pagemap_entry::data * empty_page_ptr = &* pagemap_set.begin();

std::ostream& operator<< (std::ostream &out, const pagemap_entry::data &entry)
{
  out << "AL: [";
  for (auto client: entry.ACCESSLIST_clients()) {
    out << client << ",";
  }
  out << "] WL: [";
  for (auto client: entry.WAITLIST_clients()) {
    out << client.client << ",";
  }
  out << "]\n";
}

/* internal_flush_request() requests a client to flush a page that is needed by
 * another client.  It's "internal" because it's generated by libpager itself,
 * as opposed to an "external" flush requested via pager_flush().
 */

void pager::internal_flush_request(memory_object_control_t client, vm_offset_t OFFSET)
{
  auto & record = outstanding_locks[{OFFSET, page_size}].locks[client];

  record.internal_lock_outstanding = true;
  record.count ++;

  mach_call(memory_object_lock_request(client, OFFSET, page_size, MEMORY_OBJECT_RETURN_ALL, true, VM_PROT_NO_CHANGE, pager_port()));
}


/* service_WAITLIST
 *
 * Assumes that pager is locked and pagemap entry of page to be
 * serviced is loaded into tmp_pagemap_entry.
 *
 * There should be a client on WAITLIST, or the first line will throw
 * an exception.
 */

void pager::service_WAITLIST(vm_offset_t offset, vm_offset_t data, bool allow_write_access, bool deallocate)
{
  if (terminating) return;

  auto first_client = tmp_pagemap_entry.first_WAITLIST_client();

  // std::cerr << "service_WAITLIST " << tmp_pagemap_entry;

  if (allow_write_access && first_client.write_access_requested) {
    mach_call(memory_object_data_supply(first_client.client, offset, data, page_size, deallocate, 0, PRECIOUS, 0));
    tmp_pagemap_entry.add_client_to_ACCESSLIST(first_client.client);
    tmp_pagemap_entry.pop_first_WAITLIST_client();
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(true);
  } else {
    do {
      tmp_pagemap_entry.add_client_to_ACCESSLIST(first_client.client);
      tmp_pagemap_entry.pop_first_WAITLIST_client();

      if (tmp_pagemap_entry.is_WAITLIST_empty()) {
        mach_call(memory_object_data_supply(first_client.client, offset, data, page_size,
                                            deallocate, VM_PROT_WRITE, PRECIOUS, 0));
        break;
      }

      auto next_client = tmp_pagemap_entry.first_WAITLIST_client();

      mach_call(memory_object_data_supply(first_client.client, offset, data, page_size,
                                          next_client.write_access_requested ? deallocate : false,
                                          VM_PROT_WRITE, PRECIOUS, 0));
      first_client = next_client;
    } while (! first_client.write_access_requested);
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(false);
  }

  if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
    for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
      internal_flush_request(client, offset);
    }
  }
}

// send_error_to_WAITLIST()
//
// assumes that pager is locked and tmp_pagemap_entry is loaded

void pager::send_error_to_WAITLIST(vm_offset_t OFFSET)
{
  for (auto client: tmp_pagemap_entry.WAITLIST_clients()) {
    if (tmp_pagemap_entry.is_client_on_ACCESSLIST(client.client)) {
      internal_flush_request(client.client, OFFSET);
      NEXTERROR.emplace_back(client.client, OFFSET, tmp_pagemap_entry.get_ERROR());
    } else {
      mach_call(memory_object_data_error(client.client, OFFSET, page_size, tmp_pagemap_entry.get_ERROR()));
    }
  }
  tmp_pagemap_entry.clear_WAITLIST();
}

// finalize_unlock() - call with pager lock held and tmp_pagemap_entry loaded
//
// There should be a client on WAITLIST, or the first line will throw an exception.

void pager::finalize_unlock(vm_offset_t OFFSET, kern_return_t ERROR)
{
  auto client = tmp_pagemap_entry.first_WAITLIST_client().client;

  tmp_pagemap_entry.pop_first_WAITLIST_client();

  if (ERROR == KERN_SUCCESS) {
    mach_call(memory_object_lock_request(client, OFFSET, page_size, false, false, VM_PROT_NONE, 0));
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(true);
    if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
      internal_flush_request(client, OFFSET);
    }
  } else {
    internal_flush_request(client, OFFSET);
    NEXTERROR.emplace_back(client, OFFSET, ERROR);
  }
}

void pager::data_request(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                         vm_offset_t LENGTH, vm_prot_t DESIRED_ACCESS)
{
  std::unique_lock<std::mutex> pager_lock(lock);

  assert(LENGTH == page_size);
  assert(OFFSET % page_size == 0);

  if (terminating) return;

  // fprintf(stderr, "data_request(MEMORY_CONTROL=%d, OFFSET=%d) page_size=%d\n", MEMORY_CONTROL, OFFSET, page_size);
  // fprintf(stderr, "PAGINGOUT = %d\n", pagemap[OFFSET / page_size]->get_PAGINGOUT());

  tmp_pagemap_entry = pagemap[OFFSET / page_size];

  for (auto ne = NEXTERROR.cbegin(); ne != NEXTERROR.cend(); ne ++) {
    if ((ne->client == MEMORY_CONTROL) && (ne->offset == OFFSET)) {
      mach_call(memory_object_data_error(MEMORY_CONTROL, OFFSET, LENGTH, ne->error));
      tmp_pagemap_entry.set_ERROR(ne->error);
      NEXTERROR.erase(ne);
      return;
    }
  }

  if (tmp_pagemap_entry.get_PAGINGOUT()) {
    if (tmp_pagemap_entry.is_WAITLIST_empty() && ! tmp_pagemap_entry.is_ACCESSLIST_empty()) {
      for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
        internal_flush_request(client, OFFSET);
      }
    }
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if (tmp_pagemap_entry.is_client_on_ACCESSLIST(MEMORY_CONTROL)) {
    if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
      tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
      pagemap[OFFSET / page_size] = tmp_pagemap_entry;
      internal_lock_completed(MEMORY_CONTROL, OFFSET, page_size, pager_lock);
      return;
    } else {
      tmp_pagemap_entry.remove_client_from_ACCESSLIST(MEMORY_CONTROL);
      tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(false);
    }
  }

  if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if (tmp_pagemap_entry.get_WRITE_ACCESS_GRANTED()) {
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
      internal_flush_request(client, OFFSET);
    }
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if ((DESIRED_ACCESS & VM_PROT_WRITE) && ! tmp_pagemap_entry.is_ACCESSLIST_empty()) {
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
      internal_flush_request(client, OFFSET);
    }
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if (tmp_pagemap_entry.get_INVALID()) {
    mach_call(memory_object_data_error(MEMORY_CONTROL, OFFSET, LENGTH, tmp_pagemap_entry.get_ERROR()));
  }

  tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);

  pagemap[OFFSET / page_size] = tmp_pagemap_entry;
  pager_lock.unlock();

  vm_address_t buffer;
  int write_lock;

  kern_return_t err = pager_read_page(upi, OFFSET, &buffer, &write_lock);

  pager_lock.lock();
  tmp_pagemap_entry = pagemap[OFFSET / page_size];

  tmp_pagemap_entry.set_ERROR(err);
  if (err != KERN_SUCCESS) {
    send_error_to_WAITLIST(OFFSET);
  } else {
    service_WAITLIST(OFFSET, buffer, !write_lock, true);
  }

  pagemap[OFFSET / page_size] = tmp_pagemap_entry;
}

void pager::object_init (mach_port_t control, mach_port_t name, vm_size_t pagesize)
{
  // fprintf(stderr, "object_init entering\n");
  assert (pagesize == page_size);

  std::unique_lock<std::mutex> pager_lock(lock);

  if (terminating) return;

  clients.insert(control);

  mach_port_t old;
  mach_call(mach_port_request_notification(mach_task_self(), control,
                                           MACH_NOTIFY_DEAD_NAME, 0,
                                           pager_port(),
                                           MACH_MSG_TYPE_MAKE_SEND_ONCE, &old));

  mach_call(memory_object_ready (control, may_cache, copy_strategy));
  // fprintf(stderr, "object_init exiting\n");
}

// drop_client() - call with pager lock held

void pager::drop_client (mach_port_t control, const char * const reason,
                         std::unique_lock<std::mutex> & pager_lock)

{
  // One of my test cases transmitted a data_request, then called
  // object_terminate before receiving the data_supply in reply, so we
  // had both outstanding pages and outstanding messages.  Doubt this
  // would happen with an actual Mach kernel as client, but we also
  // have to deal with malicious clients.

  // pending locks - treat them as if they returned immediately

  bool some_locks_cleared = false;

  for (auto it = outstanding_locks.begin(); it != outstanding_locks.end();) {

    bool internal_lock_required = false;

    vm_offset_t OFFSET = it->first.first;
    vm_size_t LENGTH = it->first.second;

    if (it->second.locks.count(control) > 0) {
      some_locks_cleared = true;
      internal_lock_required = it->second.locks[control].internal_lock_outstanding;
      it->second.locks.erase(control);
    }
    if (it->second.locks.empty()) {
      it->second.waiting_threads.notify_all();
      it = outstanding_locks.erase(it);
    } else {
      it ++;
    }
    if (internal_lock_required) {
      internal_lock_completed(control, OFFSET, LENGTH, pager_lock);
    }
  }

  if (some_locks_cleared) {
    fprintf(stderr, "libpager: warning: dropping client %lu (%s) with outstanding locks\n", control, reason);
  }

  // check to see if this client has any outstanding pages

  // this seems to happen a lot, even for pages with WRITE access, so
  // no warning messages are in order; just drop the client from the
  // ACCESSLISTs

  // XXX reorganize code to do this faster than searching
  // through the entire pagemap

  bool removed_from_WAITLIST = false;

  for (auto & pm: pagemap) {
    if (pm->is_client_on_ACCESSLIST(control)) {
      tmp_pagemap_entry = pm;
      tmp_pagemap_entry.remove_client_from_ACCESSLIST(control);
      tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(false);
      pm = tmp_pagemap_entry;
    }
    for (auto wl: pm->WAITLIST_clients()) {
      if (wl.client == control) {
        tmp_pagemap_entry = pm;
        auto wl2 = tmp_pagemap_entry.WAITLIST_clients();
        wl2.erase(std::remove_if(wl2.begin(), wl2.end(),
                                 [control](WAITLIST_client & x){return x.client == control;}),
                  wl2.end());
        pm = tmp_pagemap_entry;
        removed_from_WAITLIST = true;
        break;
      }
    }
  }

  if (removed_from_WAITLIST) {
    fprintf(stderr, "libpager: warning: dropping client %lu (%s) with outstanding waits\n", control, reason);
  }

  // pending attribute changes - signal them with "fake" change completed messages

  for (auto it = outstanding_change_requests.begin(); it != outstanding_change_requests.end(); it ++) {
    if (it->client == control) {
      // fprintf(stderr, "sending an m_o_change_completed\n");
      mach_call(memory_object_change_completed(it->reply, MACH_MSG_TYPE_MAKE_SEND, 0, 0));
    }
  }

  // clear dead name notification

  // If we don't do this, then after the pager's destructor is called,
  // a notification might get generated on the destroyed pager object.

  // Any message, even an ignored notification, will trigger a call
  // to ports_begin_rpc() and will require the port_info structure
  // to be valid, so we can't send messages to destroyed objects,
  // thus the need to clear the notification request now.

  // KERN_INVALID_ARGUMENT will be generated if the name is already dead.

  mach_port_t old;
  mach_call(mach_port_request_notification(mach_task_self(), control,
                                           MACH_NOTIFY_DEAD_NAME, 0,
                                           MACH_PORT_NULL,
                                           MACH_MSG_TYPE_MAKE_SEND_ONCE, &old),
            KERN_INVALID_ARGUMENT);

  clients.erase(control);
}

void pager::object_terminate (mach_port_t control, mach_port_t name)
{
  std::unique_lock<std::mutex> pager_lock(lock);

  drop_client(control, "object_terminate", pager_lock);

#if 0
  // check to see if any messages are outstanding on control port

  // this seems to happen a lot with m_o_change_attributes (2095) messages,
  // so I no longer bother to run this code

  mach_port_status_t status;
  mach_call(mach_port_get_receive_status(mach_task_self(), control, &status));
  if (status.mps_msgcount > 0) {
    fprintf(stderr, "libpager: warning: client %u called object_terminate with outstanding messages:", control);
    while (1) {
      machMessage msg;

      if (mach_call(mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, msg.max_size, control, 0, MACH_PORT_NULL),
                    MACH_RCV_TIMED_OUT) != KERN_SUCCESS) {
        break;
      }
      fprintf(stderr, " %d", msg->msgh_id);
    }
    fprintf(stderr, "\n");
  }
#endif

  // drop a client - destroy (or drop send and receive rights) on control and name

  mach_call(mach_port_destroy (mach_task_self (), control));
  mach_call(mach_port_destroy (mach_task_self (), name));

  // XXX if we lose our last client, we can free the pagemap

  // fprintf(stderr, "object_terminate done\n");
}

void pager::shutdown (void)
{
  terminating = true;

  /* Fetch and flush all pages
   *
   * pager_return() will also wait for all writes to finish.
   */

  pager_return (this, 1);

  ports_destroy_right(this);
}

void pager::lock_object(vm_offset_t OFFSET, vm_size_t LENGTH, int RETURN, bool FLUSH, bool sync)
{
  // fprintf(stderr, "lock_object(OFFSET=%d, LENGTH=%d, RETURN=%d, FLUSH=%d, SYNC=%d)\n",
  //         OFFSET, LENGTH, RETURN, FLUSH, sync);

  // sync: flush=false, return=MEMORY_OBJECT_RETURN_DIRTY
  // return: flush=true, return=MEMORY_OBJECT_RETURN_ALL
  // flush: flush=true, return=MEMORY_OBJECT_RETURN_NONE

  bool only_signal_WRITE_clients = (! FLUSH) && (RETURN != MEMORY_OBJECT_RETURN_ALL);

  // XXX allocates on heap.  Would be faster to allocate on stack
  std::set<memory_object_control_t> clients;

  std::unique_lock<std::mutex> pager_lock(lock);

  vm_size_t npages = LENGTH / page_size;

  vm_offset_t page = OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    // std::cerr << pagemap[page];
    for (auto client: pagemap[page]->ACCESSLIST_clients()) {
      if (! only_signal_WRITE_clients || pagemap[page]->get_WRITE_ACCESS_GRANTED()) {
        clients.insert(client);
      }
    }
  }

  if (! clients.empty()) {

    if (! sync) {

      for (auto client: clients) {
        mach_call(memory_object_lock_request(client, OFFSET, LENGTH, RETURN, FLUSH, VM_PROT_NO_CHANGE, 0));
      }

    } else {

      auto & record = outstanding_locks[{OFFSET, LENGTH}];

      for (auto client: clients) {
        mach_call(memory_object_lock_request(client, OFFSET, LENGTH, RETURN, FLUSH, VM_PROT_NO_CHANGE, pager_port()));
        record.locks[client].count ++;
      }

      record.waiting_threads.wait(pager_lock);

      // we now wait until any outstanding writes to complete

      // find the last data block on WRITEWAIT that overlaps this lock request
      // and wait for it to finish

      for (auto iter = WRITEWAIT.rbegin(); iter != WRITEWAIT.rend(); iter ++) {
        if ((iter->OFFSET <= OFFSET + LENGTH) && (iter->OFFSET + iter->LENGTH >= OFFSET)) {
          iter->waiting_threads.wait(pager_lock);
          break;
        }
      }
    }
  }
  // fprintf(stderr, "lock_object exiting\n");
}

void pager::change_attributes(boolean_t may_cache, memory_object_copy_strategy_t copy_strategy, int sync)
{
  // fprintf(stderr, "change_attributes(may_cache=%d, copy_strategy=%d, sync=%d)\n", may_cache, copy_strategy, sync);

  std::unique_lock<std::mutex> pager_lock(lock);

  if (! sync) {

    for (auto client: clients) {
      mach_call(memory_object_change_attributes (client, may_cache, copy_strategy, MACH_PORT_NULL));
    }

  } else {

    mach_port_t portset;
    int locks_outstanding = 0;

    // fprintf(stderr, "entering change_attributes\n");

    mach_call(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &portset));

    for (auto client: clients) {
      mach_port_t reply;
      mach_call(mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &reply));
      mach_call(memory_object_change_attributes (client, may_cache, copy_strategy, reply));
      mach_call(mach_port_move_member (mach_task_self (), reply, portset));
      outstanding_change_requests.emplace(client, reply);
      locks_outstanding ++;
    }

    while (locks_outstanding) {
      machMessage msg;

      pager_lock.unlock();
      mach_call(mach_msg (msg, MACH_RCV_MSG, 0, msg.max_size, portset, 0, MACH_PORT_NULL));
      pager_lock.lock();

      // make sure that the message is a m_o_change_completed
      assert(msg->msgh_id == 2209);

      // C++ doesn't have an easy bi-direction map class, so just
      // search through a set for a matching client

      //for (auto it: outstanding_change_requests) {
      for (auto it = outstanding_change_requests.begin(); it != outstanding_change_requests.end(); it ++) {
        if (it->reply == msg->msgh_local_port) {
          outstanding_change_requests.erase(it);
          break;
        }
      }

      // XXX perhaps we should use deallocate or mod_refs instead?
      mach_call(mach_port_destroy (mach_task_self (), msg->msgh_local_port));

      // fprintf(stderr, "change_completed from port %d\n", msg->msgh_local_port);

      locks_outstanding --;
    }

    // XXX perhaps we should use deallocate or mod_refs instead?
    mach_call(mach_port_destroy (mach_task_self (), portset));

    // fprintf(stderr, "leaving change_attributes\n");
  }
}

void pager::internal_lock_completed(memory_object_control_t MEMORY_CONTROL,
                                    vm_offset_t OFFSET, vm_size_t LENGTH,
                                    std::unique_lock<std::mutex> & pager_lock)
{
  assert(LENGTH % page_size == 0);
  assert(OFFSET % page_size == 0);

  if (terminating) return;

  vm_size_t npages = LENGTH / page_size;

  uint8_t * operation = (uint8_t *) alloca(npages);
  const int PAGEIN = 1;
  const int UNLOCK = 2;

  vm_offset_t page = OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    operation[i] = 0;
    assert(! pagemap[page]->get_WRITE_ACCESS_GRANTED());
    if (pagemap[page]->is_client_on_ACCESSLIST(MEMORY_CONTROL) && ! pagemap[page]->is_WAITLIST_empty()) {
      tmp_pagemap_entry = pagemap[page];
      tmp_pagemap_entry.remove_client_from_ACCESSLIST(MEMORY_CONTROL);
      if (tmp_pagemap_entry.is_ACCESSLIST_empty()) {
        if (! tmp_pagemap_entry.get_INVALID()) {
          operation[i] = PAGEIN;
        } else {
          send_error_to_WAITLIST(page * page_size);
        }
      } else if ((tmp_pagemap_entry.ACCESSLIST_num_clients() == 1)
                 && tmp_pagemap_entry.is_client_on_ACCESSLIST(tmp_pagemap_entry.first_WAITLIST_client().client)) {
        operation[i] = UNLOCK;
      }
      pagemap[page] = tmp_pagemap_entry;
    }
  }

  pager_lock.unlock();

  vm_address_t * buffer = (vm_address_t *) alloca(npages * sizeof(vm_address_t));
  int * write_lock = (int *) alloca(npages * sizeof(int));
  kern_return_t * err = (kern_return_t *) alloca(npages * sizeof(kern_return_t));

  page = OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    if (operation[i] == PAGEIN) {
      err[i] = pager_read_page(upi, page * page_size, &buffer[i], &write_lock[i]);
    } else if (operation[i] == UNLOCK) {
      err[i] = pager_unlock_page(upi, page * page_size);
    }
  }

  pager_lock.lock();

  page = OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    if (operation[i] == PAGEIN) {
      tmp_pagemap_entry = pagemap[page];
      tmp_pagemap_entry.set_ERROR(err[i]);
      if (err[i] == KERN_SUCCESS) {
        service_WAITLIST(page * page_size, buffer[i], !write_lock[i], true);
      } else {
        send_error_to_WAITLIST(page * page_size);
      }
      pagemap[page] = tmp_pagemap_entry;
    } else if (operation[i] == UNLOCK) {
      tmp_pagemap_entry = pagemap[page];
      finalize_unlock(page * page_size, err[i]);
      pagemap[page] = tmp_pagemap_entry;
    }
  }
}

void pager::lock_completed(memory_object_control_t MEMORY_CONTROL,
                           vm_offset_t OFFSET, vm_size_t LENGTH)
{
  std::unique_lock<std::mutex> pager_lock(lock);

  auto it = outstanding_locks.find({OFFSET, LENGTH});

  assert (it != outstanding_locks.end());

  auto & record = it->second;

  bool internal_lock_required = false;

  assert(record.locks[MEMORY_CONTROL].count > 0);

  record.locks[MEMORY_CONTROL].count --;

  if (record.locks[MEMORY_CONTROL].count == 0) {
    internal_lock_required = record.locks[MEMORY_CONTROL].internal_lock_outstanding;
    record.locks.erase(MEMORY_CONTROL);
  }

  if (record.locks.empty()) {
    record.waiting_threads.notify_all();
    outstanding_locks.erase(it);
  }

  // wait until the end of the function to call internal_lock_completed,
  // because it might send more lock requests and thus modify outstanding_locks

  if (internal_lock_required) {
    internal_lock_completed(MEMORY_CONTROL, OFFSET, LENGTH, pager_lock);
  }
}

void pager::data_unlock(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                   vm_size_t LENGTH, vm_prot_t DESIRED_ACCESS)
{
  std::unique_lock<std::mutex> pager_lock(lock);

  assert(LENGTH % page_size == 0);
  assert(OFFSET % page_size == 0);

  if (terminating) return;

  vm_size_t npages = LENGTH / page_size;

  bool * do_unlock = (bool *) alloca(npages * sizeof(bool));

  vm_offset_t page = OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    do_unlock[i] = false;
    tmp_pagemap_entry = pagemap[page];
    assert(tmp_pagemap_entry.is_client_on_ACCESSLIST(MEMORY_CONTROL));
    assert(! tmp_pagemap_entry.get_WRITE_ACCESS_GRANTED());
    assert(! tmp_pagemap_entry.get_PAGINGOUT());
    if (tmp_pagemap_entry.is_any_client_waiting_for_write_access()) {
    } else if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
      tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, true);
    } else if (tmp_pagemap_entry.ACCESSLIST_num_clients() > 1) {
      tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, true);
      for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
        internal_flush_request(client, OFFSET);
      }
    } else {
      tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, true);
      do_unlock[i] = true;
    }
    pagemap[page] = tmp_pagemap_entry;
  }

  pager_lock.unlock();

  kern_return_t * err = (kern_return_t *) alloca(npages * sizeof(kern_return_t));

  page = OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    if (do_unlock[i]) {
      err[i] = pager_unlock_page(upi, page * page_size);
    }
  }

  pager_lock.lock();

  page = OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    if (do_unlock[i]) {
      tmp_pagemap_entry = pagemap[page];
      finalize_unlock(page * page_size, err[i]);
      pagemap[page] = tmp_pagemap_entry;
    }
  }
}

// call with pager locked

void pager::service_first_WRITEWAIT_entry(std::unique_lock<std::mutex> & pager_lock)
{
  auto & current = WRITEWAIT.front();
  vm_size_t npages = current.LENGTH / page_size;

  // fprintf(stderr, "service_first_WRITEWAIT_entry(OFFSET=%d, LENGTH=%d)\n", current.OFFSET, current.LENGTH);

  auto matching_page_count_on_WRITEWAIT = [this] (vm_offset_t page) -> int
    {
      int count = 0;

      for (auto & next: WRITEWAIT) {
        if ((next.OFFSET <= page * page_size) && (next.OFFSET + next.LENGTH >= (page + 1) * page_size)) {
          count ++;
        }
      }

      return count;
    };

  bool * do_pageout = (bool *) alloca(npages * sizeof(bool));
  bool any_pageout_required = false;

  vm_offset_t page = current.OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    do_pageout[i] = (matching_page_count_on_WRITEWAIT(page) == 1);
    if (do_pageout[i]) {
      any_pageout_required = true;
    }
  }

  kern_return_t * err = (kern_return_t *) alloca(npages * sizeof(kern_return_t));

  // fprintf(stderr, "service_first_WRITEWAIT_entry pageout_required=%d\n", any_pageout_required);

  if (any_pageout_required) {

    pager_lock.unlock();

    page = current.OFFSET / page_size;
    for (vm_size_t i = 0; i < npages; i ++, page ++) {
      if (do_pageout[i]) {
        err[i] = pager_write_page(upi, page * page_size, current.DATA + i * page_size);
      }
    }

    pager_lock.lock();

  }

  bool * do_notify = (bool *) alloca(npages * sizeof(bool));
  bool any_notification_required = false;

  page = current.OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    do_notify[i] = false;
    if (do_pageout[i]) {
      tmp_pagemap_entry = pagemap[page];
      if (err[i] == KERN_SUCCESS) {
        tmp_pagemap_entry.set_INVALID(false);
      } else {
        tmp_pagemap_entry.set_INVALID(true);
        tmp_pagemap_entry.set_ERROR(err[i]);
      }
      if (matching_page_count_on_WRITEWAIT(page) == 1) {
        tmp_pagemap_entry.set_PAGINGOUT(false);
        if (! current.KERNEL_COPY) {
          if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
            service_WAITLIST(page * page_size, current.DATA + i * page_size, true, false);
          } else {
            do_notify[i] = true;
            any_notification_required = true;

            // At this point, the older libpager code contained the following comment:
            //
            // "Clear any error that is left.  Notification on eviction
            //  is used only to change association of page, so any
            //  error may no longer be valid."
            //
            // I'm not sure just what this means, but mimic this behavior here.
            //
            // The old code didn't clear INVALID, but I don't see how it makes sense not to.

            tmp_pagemap_entry.set_INVALID(false);
            tmp_pagemap_entry.set_ERROR(KERN_SUCCESS);

            NEXTERROR.erase(std::remove_if(NEXTERROR.begin(), NEXTERROR.end(),
                                           [this, page](NEXTERROR_entry & x){return x.offset == page * page_size;}),
                            NEXTERROR.end());
          }
        }
      } else {
        assert(current.KERNEL_COPY);
      }
      pagemap[page] = tmp_pagemap_entry;
    }
  }

  munmap((void *) current.DATA, current.LENGTH);

  if (notify_on_evict && any_notification_required) {
    pager_lock.unlock();

    page = current.OFFSET / page_size;
    for (vm_size_t i = 0; i < npages; i ++, page ++) {
      if (do_notify[i]) {
        pager_notify_evict(upi, page * page_size);
      }
    }

    pager_lock.lock();
  }

  current.waiting_threads.notify_all();
  WRITEWAIT.pop_front();
  // fprintf(stderr, "service_first_WRITEWAIT_entry pop_front\n");
}

void pager::data_return(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                        vm_offset_t DATA, vm_size_t LENGTH, boolean_t DIRTY,
                        boolean_t KERNEL_COPY)
{
  std::unique_lock<std::mutex> pager_lock(lock);

  assert(LENGTH % page_size == 0);
  assert(OFFSET % page_size == 0);

  vm_size_t npages = LENGTH / page_size;

  bool * do_unlock = (bool *) alloca(npages * sizeof(bool));
  bool * do_notify = (bool *) alloca(npages * sizeof(bool));
  bool any_unlocks_or_notifies_required = false;

  // fprintf(stderr, "data_return(MEMORY_CONTROL=%d, OFFSET=%d, LENGTH=%d, DIRTY=%d, KCOPY=%d, count=%d)\n",
  //         MEMORY_CONTROL, OFFSET, LENGTH, DIRTY, KERNEL_COPY, *((int *)DATA));

  vm_offset_t page = OFFSET / page_size;
  for (vm_size_t i = 0; i < npages; i ++, page ++) {
    do_unlock[i] = false;
    tmp_pagemap_entry = pagemap[page];
    if (! KERNEL_COPY) {
      if (MEMORY_CONTROL != MACH_PORT_DEAD) {
        tmp_pagemap_entry.remove_client_from_ACCESSLIST(MEMORY_CONTROL);
      }
      if (tmp_pagemap_entry.is_ACCESSLIST_empty() && ! tmp_pagemap_entry.is_WAITLIST_empty()) {
        service_WAITLIST(page * page_size, DATA + i * page_size, true, false);
      } else if ((tmp_pagemap_entry.ACCESSLIST_num_clients() == 1)
                 && ! tmp_pagemap_entry.get_WRITE_ACCESS_GRANTED()
                 && ! tmp_pagemap_entry.is_WAITLIST_empty()
                 && tmp_pagemap_entry.is_client_on_ACCESSLIST(tmp_pagemap_entry.first_WAITLIST_client().client)
                 && ! terminating) {
        do_unlock[i] = true;
        any_unlocks_or_notifies_required = true;
      } else if (tmp_pagemap_entry.is_ACCESSLIST_empty() && tmp_pagemap_entry.is_WAITLIST_empty() && ! DIRTY) {
        do_notify[i] = true;
        any_unlocks_or_notifies_required = true;

        tmp_pagemap_entry.set_INVALID(false);
        tmp_pagemap_entry.set_ERROR(KERN_SUCCESS);

        NEXTERROR.erase(std::remove_if(NEXTERROR.begin(), NEXTERROR.end(),
                                       [this, page](NEXTERROR_entry & x){return x.offset == page * page_size;}),
                        NEXTERROR.end());
      }
    }
    if (DIRTY) {
      tmp_pagemap_entry.set_PAGINGOUT(true);
    }
    pagemap[page] = tmp_pagemap_entry;

    // std::cerr << pagemap[page];
  }

  if (any_unlocks_or_notifies_required) {
    assert (!DIRTY);

    pager_lock.unlock();

    kern_return_t * err = (kern_return_t *) alloca(npages * sizeof(kern_return_t));

    page = OFFSET / page_size;
    for (vm_size_t i = 0; i < npages; i ++, page ++) {
      if (do_unlock[i]) {
        err[i] = pager_unlock_page(upi, page * page_size);
      }
      if (do_notify[i]) {
        pager_notify_evict(upi, page * page_size);
      }
    }

    pager_lock.lock();

    page = OFFSET / page_size;
    for (vm_size_t i = 0; i < npages; i ++, page ++) {
      if (do_unlock[i]) {
        tmp_pagemap_entry = pagemap[page];
        finalize_unlock(page * page_size, err[i]);
        pagemap[page] = tmp_pagemap_entry;
      }
    }
  }

  if (DIRTY) {
    if (! WRITEWAIT.empty()) {
      WRITEWAIT.emplace_back(OFFSET, DATA, LENGTH, KERNEL_COPY);
    } else {
      WRITEWAIT.emplace_back(OFFSET, DATA, LENGTH, KERNEL_COPY);
      while (! WRITEWAIT.empty()) {
        service_first_WRITEWAIT_entry(pager_lock);
      }
    }
  } else {
    munmap((void *) DATA, LENGTH);
  }
}

kern_return_t pager::get_error(vm_offset_t OFFSET)
{
  return pagemap[OFFSET / page_size]->get_ERROR();
}

void pager::dead_name(mach_port_t DEADNAME)
{
  std::unique_lock<std::mutex> pager_lock(lock);

  drop_client(DEADNAME, "dead name", pager_lock);
}

pager::~pager()
{
  std::unique_lock<std::mutex> pager_lock(lock);

  if (! clients.empty()) {
    // This can happen if a rogue client calls m_o_init, then
    // terminates without calling m_o_terminate.  We'll get both a NO
    // SENDERS notification on the pager port, and a DEAD NAME
    // notification on the client's control port.  If the the NO
    // SENDERS gets processed first, and there are no other clients,
    // we'll end up here.

    // It can also happen after a pager_shutdown() with outstanding
    // clients.

    for (auto cl: clients) {
      drop_client(cl, "~pager", pager_lock);
    }
  }

  if (! WRITEWAIT.empty()) {
    WRITEWAIT.back().waiting_threads.wait(pager_lock);
  }

  assert(outstanding_locks.empty());
  assert(outstanding_change_requests.empty());

  pager_clear_user_data (upi);
}
