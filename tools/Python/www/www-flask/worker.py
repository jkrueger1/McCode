from app import db_session
from models import *
from subprocess import Popen, PIPE
from os.path import basename, dirname, splitext, abspath

import time, os, shutil, re, json
import traceback
import tarfile

from config import IMAGE_FORMAT, MPI_NP, DATA_FILES, \
     MAX_RAY_SAMPLES, MAX_SCAN_POINTS


SIM_SRC_PATH = "sim/%s.instr"
SIM_BIN_PATH = "sim/%s.out"
SIM_C_PATH = "sim/%s.c"

WORK_PATH = "out/%s"

STACKTRACE = '''Python stack-trace:
%s'''


def popenwait(args, cwd, log):
    pid = Popen(args, cwd=cwd, stdout=PIPE, stderr=PIPE)
    out, err = pid.communicate()
    log(out, err)
    return out


# try to use new R plotter instead of mcplot
def mcplot(simfile, outfile, logger, logy=False):
    ''' Plot a mcstas.sim file with mcplot '''
    args = (["mcplot", "-%s" % IMAGE_FORMAT] +
            (logy and ["-log"] or []) +
            [basename(simfile)])
    popenwait(args, cwd=dirname(simfile), log=logger)
    os.rename("%s.%s" % (simfile, IMAGE_FORMAT) , outfile)

try:
    from rplot.plot import plotSim
    def plot(simfile, outfile, logger, logy=False):
        ''' Plot a sim file with R '''
        try:
            plotSim(simfile, logy, IMAGE_FORMAT)
            os.rename('%s.%s' % (simfile, IMAGE_FORMAT), outfile)
        except Exception,e:
            print 'Fallback to mcplot, because:'
            traceback.print_exc()
            mcplot(simfile, outfile, logger=logger, logy=logy)
    print 'INFO: using plotter from rplot/'
except Exception, e:
    # Error occured: Print it and fallback to old mcplot
    print e
    plot = mcplot


def display(instr, params, outfile, log, fmt="png"):
    ''' Display instrument '''
    # VRML needs --format, which does not seem to work with gif/ps
    fmt_arg = fmt == 'vrml' and '--format=vrml' or '-'+fmt
    args = (["mcdisplay", "-k", "--save", fmt_arg, basename(instr),
             "-n", str(1) # precision (iterations)
             ] + params)
    popenwait(args, cwd=dirname(instr), log=log)
    # VRML needs special treatment
    if fmt == 'vrml':
        mcout = '%s/%s' % (dirname(instr), 'mcdisplay_commands.wrl')
    else:
        mcout = splitext(instr)[0] + ".out." + fmt
    os.rename(mcout, outfile)


def value_to_str(val):
    ''' Converts a value to string, handling lists specially '''
    if isinstance(val, list):
        return ','.join(value_to_str(x) for x in val)
    return str(val)


def first_range(val):
    ''' Select the first element of a range, e.g. lambda=0.7,1.2 -> lambda=0.7 '''
    return [ re.sub(r',[^\s]+', r'', x) for x in val ]


def work():
    ''' Fetch and process a job by running the McStas simulation '''
    # fetch job
    run = SimRun.query.filter_by(status="waiting").order_by('created').first()
    if run is None:
        return
    run.status = "running"
    db_session.commit()

    print "Running job: ", run.id

    # create output folder, this works as a lock
    workdir = "%s/%%s" % (WORK_PATH % run.id)
    try:
        os.mkdir(workdir % "")
    except OSError:
        # Someone else beat us to it, bail out
        print "Skipping: already running."
        return

    # try to process job
    try:
        processJob(run, workdir)
    except:
        exc = traceback.format_exc()
        file(workdir % 'err.txt', 'a').write('\n' + STACKTRACE % exc)
        print exc

    # mark job as completed
    run.status = "done"
    db_session.commit()
    print "Done."



def processJob(run, workdir):
    ''' Process a specific job '''

    def appendLog(out, err):
        # populate result folder
        if out: file(workdir % "out.txt", "a").write(out)
        if err: file(workdir % "err.txt", "a").write(err)

    # pick seed and samples
    params = run.params

    seed = params["_seed"]
    samples = min(MAX_RAY_SAMPLES, params["_samples"])
    npoints = min(MAX_SCAN_POINTS, params["_npoints"])

    # instrument name
    name = run.sim.name
    siminstr = SIM_SRC_PATH % name
    simbin = SIM_BIN_PATH % name
    simc = SIM_C_PATH % name

    # Create hard links to instrument source, c-code and binary
    for path in (siminstr, simbin, simc):
        os.link(path, workdir % basename(path))

    # Create soft links to data files/folders
    for path in DATA_FILES:
        for fname in os.listdir(path):
            os.symlink('%s/%s' % (abspath(path), fname), workdir % fname)

    # compute instrument layout
    is_scan = any(isinstance(param, list) for param in params.values())

    # generate list of parameters
    params_strs = [ '%s=%s' % (str(k), value_to_str(v)) for k, v in params.items()
                    if not k[0] == '_' ]

    # Generate instrument graphics with mcdisplay
    display(workdir % (name + ".instr"), first_range(params_strs),
            workdir % "layout.gif", log=appendLog)
    display(workdir % (name + ".instr"), first_range(params_strs),
            workdir % "layout.wrl", log=appendLog, fmt='vrml')

    # run mcstas via mcrun
    args = ["mcrun"] + \
           (seed > 0   and ["--seed", str(seed)]    or []) + \
           (is_scan    and ["-N",     str(npoints)] or []) + \
           (MPI_NP > 0 and ["--mpi=%s" % MPI_NP]    or []) + \
           ["--ncount", str(samples),
            "--dir",    "mcstas",
            name] + params_strs
    popenwait(args, cwd=workdir % '', log=appendLog)

    # tar results
    tarname = 'mcstas-' + run.id
    tarf = workdir % ('%s.tar.gz' % tarname)
    tarfile.open(tarf, 'w:gz').add(workdir % "mcstas", arcname=tarname)

    def process_components(sim_path):
        # dump components
        comps = re.findall(r'filename:[ \t]*([^\s]+)',
                           file(sim_path).read())
        file(dirname(sim_path) + "/comps.json", "w").write(json.dumps(comps))
        # plot components
        for comp in comps:
            for mode in ("lin", "log"):
                plot(dirname(sim_path) + '/' + comp,
                     outfile=dirname(sim_path) + ('/plot-%s-%s.gif' % (comp, mode)),
                     logger=appendLog,
                     logy=(mode == "log"))

    os.path.walk(workdir % 'mcstas',
                 lambda _arg, folder, files:
                 process_components(folder + "/mcstas.sim"),
                 [])


if __name__ == '__main__':
    while True:
        time.sleep(1)
        try:
            work()
        except:
            # rollback any non-committed transactions
            db_session.rollback()
            # print error message to stdout
            print 'Job failed because:'
            traceback.print_exc()
