#!/usr/bin/env python2

cmds = {
    "dump_hp_entry": [
        "table_dump_entry_from_key src_ip_prio 10.0.1.1/32"
    ],
    "prio_enable": [
        "table_modify src_ip_prio set_priority 0 6 7500000 7000",
        "table_modify src_ip_prio set_priority 1 5 2500000 7000",
        "table_modify src_ip_prio set_priority 2 4 2500000 7000"
    ],
    "prio_disable": [
        "table_modify src_ip_prio set_priority 0 6 4166666 7000",
        "table_modify src_ip_prio set_priority 1 5 4166666 7000",
        "table_modify src_ip_prio set_priority 2 4 4166666 7000"
    ],
    "prio_enable_10": [
        "table_modify src_ip_prio set_priority 0 6 750000 7000",
        "table_modify src_ip_prio set_priority 1 5 250000 7000",
        "table_modify src_ip_prio set_priority 2 4 250000 7000"
    ],
    "prio_disable_10": [
        "table_modify src_ip_prio set_priority 0 6 416666 7000",
        "table_modify src_ip_prio set_priority 1 5 416666 7000",
        "table_modify src_ip_prio set_priority 2 4 416666 7000"
    ],
    "prio_enable_50": [
        "table_modify src_ip_prio set_priority 0 6 3750000 7000",
        "table_modify src_ip_prio set_priority 1 5 1250000 7000",
        "table_modify src_ip_prio set_priority 2 4 1250000 7000"
    ],
    "prio_disable_50": [
        "table_modify src_ip_prio set_priority 0 6 2083333 7000",
        "table_modify src_ip_prio set_priority 1 5 2083333 7000",
        "table_modify src_ip_prio set_priority 2 4 2083333 7000"
    ],


    "hp_inject_enable": [
        "table_modify src_ip_prio set_priority 1 5 2500000 7000",
        "table_modify src_ip_prio set_priority 2 4 2500000 7000"
    ],
    "hp_inject_disable": [
        "table_modify src_ip_prio set_priority 1 5 6250000 7000",
        "table_modify src_ip_prio set_priority 2 4 6250000 7000"
    ],
    "hp_inject_enable_10": [
        "table_modify src_ip_prio set_priority 1 5 250000 7000",
        "table_modify src_ip_prio set_priority 2 4 250000 7000"
    ],
    "hp_inject_disable_10": [
        "table_modify src_ip_prio set_priority 1 5 625000 7000",
        "table_modify src_ip_prio set_priority 2 4 625000 7000"
    ],
    "hp_inject_enable_50": [
        "table_modify src_ip_prio set_priority 1 5 1250000 7000",
        "table_modify src_ip_prio set_priority 2 4 1250000 7000"
    ],
    "hp_inject_disable_50": [
        "table_modify src_ip_prio set_priority 1 5 3125000 7000",
        "table_modify src_ip_prio set_priority 2 4 3125000 7000"
    ]
}
