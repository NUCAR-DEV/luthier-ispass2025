from yacs.config import CfgNode
import pathlib
import io
import selectors
import subprocess
import sys


def read_yaml_cfg() -> CfgNode:
    cfg = CfgNode()
    cfg.set_new_allowed(True)
    cfg.merge_from_file(str(pathlib.Path(__file__).parent.resolve().joinpath("specs.yaml")))
    cfg.freeze()
    return cfg


def capture_subprocess_output(**subprocess_args):
    """
    From https://gist.github.com/nawatts/e2cdca610463200c12eac2a14efc0bfb
    """
    # Start subprocess
    # bufsize = 1 means output is line buffered
    # universal_newlines = True is required for line buffering
    dump_stdout_stderr = False
    if "dump_stdout_stderr" in subprocess_args:
        dump_stdout_stderr = subprocess_args["dump_stdout_stderr"]
        del subprocess_args["dump_stdout_stderr"]
    process = subprocess.Popen(**subprocess_args,
                               bufsize=1,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE,
                               universal_newlines=True)

    # Create callback function for process output
    buf_out = io.StringIO()
    buf_err = io.StringIO()

    def handle_output_out(stream, mask):
        # Because the process' output is line buffered, there's only ever one
        # line to read when this function is called
        line = stream.readline()
        buf_out.write(line)
        if dump_stdout_stderr:
            sys.stdout.write(line)

    def handle_output_err(stream, mask):
        # Because the process' output is line buffered, there's only ever one
        # line to read when this function is called
        line = stream.readline()
        buf_err.write(line)
        if dump_stdout_stderr:
            sys.stderr.write(line)

    # Register callback for an "available for read" event from subprocess' stdout stream
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ, handle_output_out)
    selector.register(process.stderr, selectors.EVENT_READ, handle_output_err)

    # Loop until subprocess is terminated
    while process.poll() is None:
        # Wait for events and handle them with their registered callbacks
        events = selector.select()
        for key, mask in events:
            callback = key.data
            callback(key.fileobj, mask)

    while True:
        line = process.stdout.readline()
        if not line:
            break
        buf_out.write(line)
        sys.stdout.write(line)
    while True:
        line = process.stderr.readline()
        if not line:
            break
        buf_err.write(line)
        sys.stderr.write(line)

    # Get process return code
    return_code = process.wait()
    selector.close()

    # Store buffered stdout and stderr
    out = buf_out.getvalue()
    buf_out.close()

    err = buf_err.getvalue()
    buf_err.close()

    return return_code, out, err
