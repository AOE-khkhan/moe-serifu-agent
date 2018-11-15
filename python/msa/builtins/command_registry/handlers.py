
from msa.core.event_handler import EventHandler

from msa.builtins.command_registry.events import RegisterCommandEvent, HelpCommandEvent
from msa.builtins.tty.events import TextInputEvent
from msa.core import supervisor

import sys

class CommandRegistryHandler(EventHandler):
    """This command registers and dispatches commands. When creating a new command, it must create a
    RegisterCommandEvent. When the user enters text, the command registry handler attempts to parse the text as commands
    and dispatches command events appropriately. All command events should subclass the CommandEvent type."""

    def __init__(self, loop, event_queue):
        super().__init__(loop, event_queue)

        self.registered_commands = {}

    async def handle(self):
        _, event = await self.event_queue.get()


        if not event.propagate:
            return

        if isinstance(event, RegisterCommandEvent):
            self.register_command(event.data)

        elif isinstance(event, TextInputEvent):
            self.parse_text_input(event)

    def register_command(self, data):
        """Registers a new command"""

        # verify that no other commands utilize the same invoke keyword
        keyword = data["invoke"].lower()
        if keyword in self.registered_commands:
            print("Command with invoke keyword '{}' already registered".format(keyword))
            return

        self.registered_commands[keyword] = data

    def parse_text_input(self, event):
        """Attempts to parse text as a command, an dispatches a new command event appropriately."""
        raw_text = event.data.get("message")

        if raw_text is None or not len(raw_text):
            return

        tokens = raw_text.split()
        invoke_keyword = tokens[0]

        # search registered commands for one that corresponds to invoke_keyword
        for command_type, command_data in self.registered_commands.items():
            if invoke_keyword == command_type:
                new_event = command_data["event_constructor"]()
                new_event.init(data={
                    "raw_text": raw_text,
                    "tokens": tokens[1:len(tokens)]
                })

                supervisor.fire_event(new_event)
                return


class HelpCommandHandler(EventHandler):
    """This handler listens for RegiserCommandEvents and records registered commands. When a help command is issued, it
    prints the appropriate help text."""

    def __init__(self, loop, event_queue):
        super().__init__(loop, event_queue)

        self.registered_commands = {}

    async def init(self):
        event = RegisterCommandEvent()
        event.init({
            "event_constructor": HelpCommandEvent,
            "invoke": "help",
            "describe": "Prints available commands and information about command usage.",
            "usage": "'help'  or 'help [command name]'"
        })

        supervisor.fire_event(event)

    async def handle(self):
        _, event = await self.event_queue.get()

        if not event.propagate:
            return

        if isinstance(event, RegisterCommandEvent):
            self.register_command(event)

        elif isinstance(event, HelpCommandEvent):
            self.display_help(event)

    def register_command(self, event):
        """Registers a new command, registered commands are used for resolving help information."""
        data = event.data

        # verify that no other commands utilize the same invoke keyword
        keyword = data["invoke"].lower()
        if keyword in self.registered_commands:
            print("Command with invoke keyword '{}' already registered".format(keyword))
            return

        self.registered_commands[keyword] = data

    def display_help(self, event):
        """Displays help text overview or specific help text if a command is specified."""
        tokens = event.data.get("tokens")

        if tokens is None or not len(tokens):
            # print availiable commands
            out = "Available Commands: \n"

            for command_name, command_data in self.registered_commands.items():
                out += "{}: {}\n".format(command_name, command_data["describe"])
            out += "\n"

            self.print(out)  # TODO refactor to use TTY output event

        else:
            command = tokens[0]

            for command_name, command_data in self.registered_commands.items():
                if command == command_name:
                    out = "Help text for command '{}':\nUsage: {}\nOptions: {}\nDescription: {}\n".format(
                        command_name,
                        command_data["usage"],
                        command_data["options"],
                        command_data["describe"],
                    )

                    self.print(out)  # TODO refactor to use TTY output event
                    return

    def print(self, msg):
        """temportary work around to allow unit testing, should instead create TTy out event"""
        print(msg)
