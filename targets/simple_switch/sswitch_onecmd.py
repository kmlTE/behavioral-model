#!/usr/bin/env python2

import sswitch_CLI
import runtime_CLI

import sys
import os


def main():
    arg_parser = runtime_CLI.get_parser()
    arg_parser.add_argument('--onecmd', help='Cmd to execute',
                            type=str, action="store", default='help')

    args = arg_parser.parse_args()

    args.pre = runtime_CLI.PreType.SimplePreLAG

    services = runtime_CLI.RuntimeAPI.get_thrift_services(args.pre)
    services.extend(sswitch_CLI.SimpleSwitchAPI.get_thrift_services())

    standard_client, mc_client, sswitch_client = runtime_CLI.thrift_connect(
        args.thrift_ip, args.thrift_port, services
    )

    runtime_CLI.load_json_config(standard_client, args.json)

    ss_api = sswitch_CLI.SimpleSwitchAPI(args.pre, standard_client, mc_client, sswitch_client)

    import sswitch_commands

    for cmd in sswitch_commands.cmds[args.onecmd]:
        ss_api.onecmd(cmd)

if __name__ == '__main__':
    main()
