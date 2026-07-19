#pragma once

// 这里只做前置声明，避免把完整lwIP头文件注入所有lwIP源码
struct pbuf;
struct netif;

// access_filter中实现的IPv4输入过滤函数
int access_filter_lwip_ip4_input(struct pbuf *packet, struct netif *input_netif);

// 告诉lwIP：每个IPv4输入包都先调用我们的过滤函数

#define LWIP_HOOK_IP4_INPUT(packet, input_netif) access_filter_lwip_ip4_input((packet),(input_netif))
