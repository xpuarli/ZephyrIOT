/*
 * Copyright (c) 2016 Nordic Semiconductor ASA
 * Copyright (c) 2016 Vinayak Kariappa Chettimada
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

#ifndef _HCI_CONTROLLER_H_
#define _HCI_CONTROLLER_H_

int hci_cmd_handle(struct net_buf *cmd, struct net_buf *evt);
int hci_acl_handle(struct net_buf *acl);
void hci_evt_encode(struct radio_pdu_node_rx *node_rx, struct net_buf *buf);
void hci_acl_encode(struct radio_pdu_node_rx *node_rx, struct net_buf *buf);
void hci_num_cmplt_encode(struct net_buf *buf, uint16_t handle, uint8_t num);
bool hci_evt_is_discardable(struct radio_pdu_node_rx *node_rx);
#endif /* _HCI_CONTROLLER_H_ */
