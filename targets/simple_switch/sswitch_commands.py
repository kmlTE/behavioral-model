#!/usr/bin/env python2

cmds = {
    "dump_hp_entry": [
        "table_dump_entry_from_key src_ip_prio 10.0.1.1/32"
    ],
    "prio_enable": [
        "table_modify src_ip_prio set_priority 0 7 7500000 7000"
    ],
    "prio_disable": [
        "table_modify src_ip_prio set_priority 0 0 7500000 7000"
    ],
    "prio_enable_10": [
        "table_modify src_ip_prio set_priority 0 7 750000 7000"
    ],
    "prio_disable_10": [
        "table_modify src_ip_prio set_priority 0 0 750000 7000"
    ],
    "prio_enable_50": [
        "table_modify src_ip_prio set_priority 0 7 3750000 7000"
    ],
    "prio_disable_50": [
        "table_modify src_ip_prio set_priority 0 0 3750000 7000"
    ],
    
    "hp_inject_enable": [],
    "hp_inject_disable": [],
    "hp_inject_enable_10": [],
    "hp_inject_disable_10": [],
    "hp_inject_enable_50": [],
    "hp_inject_disable_50": []
}
