#!/bin/bash
#
# File:     pbs_submit.sh
# Author:   David Rebatto (david.rebatto@mi.infn.it)
#
# Revision history:
#     8-Apr-2004: Original release
#    28-Apr-2004: Patched to handle arguments with spaces within (F. Prelz)
#                 -d debug option added (print the wrapper to stderr without submitting)
#    10-May-2004: Patched to handle environment with spaces, commas and equals
#    13-May-2004: Added cleanup of temporary file when successfully submitted
#    18-May-2004: Search job by name in log file (instead of searching by jobid)
#     8-Jul-2004: Try a chmod u+x on the file shipped as executable
#                 -w option added (cd into submission directory)
#    20-Sep-2004: -q option added (queue selection)
# 
#
# Description:
#   Submission script for PBS, to be invoked by blahpd server.
#   Usage:
#     pbs_submit.sh -c <command> [-i <stdin>] [-o <stdout>] [-e <stderr>] [-w working dir] [-- command's arguments]
#
#
#  Copyright (c) 2004 Istituto Nazionale di Fisica Nucleare (INFN).
#  All rights reserved.
#  See http://grid.infn.it/grid/license.html for license details.
#

usage_string="Usage: $0 -c <command> [-i <stdin>] [-o <stdout>] [-e <stderr>] [-v <environment>] [-s <yes | no>] [-- command_arguments]"
logpath=/var/spool/pbs/server_logs

stgcmd="yes"
stgproxy="yes"

###############################################################
# Parse parameters
###############################################################

while getopts "i:o:e:c:s:v:dw:q:" arg 
do
    case "$arg" in
    i) stdin="$OPTARG" ;;
    o) stdout="$OPTARG" ;;
    e) stderr="$OPTARG" ;;
    v) envir="$OPTARG";;
    c) the_command="$OPTARG" ;;
    s) stgcmd="$OPTARG" ;;
    d) debug="yes" ;;
    w) workdir="$OPTARG";;
    q) queue="$OPTARG";;
    -) break ;;
    ?) echo $usage_string
       exit 1 ;;
    esac
done

# Command is mandatory
if [ "x$the_command" == "x" ]
then
    echo $usage_string
    exit 1
fi

shift `expr $OPTIND - 1`
arguments=$*

###############################################################
# Create wrapper script
###############################################################

# Get a suitable name for temp file
if [ "x$debug" != "xyes" ]
then
    tmp_file=`mktemp -q blahjob_XXXXXX`
    if [ $? -ne 0 ]; then
        echo Error
        exit 1
    fi
else
    # Just print to stderr if in debug
    tmp_file="/proc/$$/fd/2"
fi

# Write wrapper preamble
cat > $tmp_file << end_of_preamble
#!/bin/bash
# PBS job wrapper generated by `basename $0`
# on `/bin/date`
#
# PBS directives:
#PBS -S /bin/bash
end_of_preamble

# Write PBS directives according to command line options
[ -z "$stdin" ]  || arguments="$arguments < $stdin"
[ -z "$stdout" ] || echo "#PBS -o $stdout" >> $tmp_file
[ -z "$stderr" ] || echo "#PBS -e $stderr" >> $tmp_file
[ -z "$queue" ] || echo "#PBS -q $queue" >> $tmp_file
[ -z "$stgcmd" ] || blahpd_inputsandbox="`basename $the_command`@`hostname -f`:$the_command"

proxy_string=`echo ';'$envir | sed -e 's/.*;[^X]*X509_USER_PROXY[^=]*\= *\([^\; ]*\).*/\1/'`

need_to_reset_proxy=no
proxy_remote_file=

if [ "x$stgproxy" == "xyes" ] && [ "x$proxy_string" != "x" ] ; then
    if [ "x$workdir" != "x" ] ; then
        proxy_local_file=${workdir}"/"`basename $proxy_string`;
    else
        proxy_local_file=$proxy_string;
    fi
    if [ -r $proxy_local_file ] ; then
        if [ ! -z $blahpd_inputsandbox ]; then blahpd_inputsandbox="${blahpd_inputsandbox},"; fi
        proxy_remote_file=${tmp_file}.proxy
        blahpd_inputsandbox="${blahpd_inputsandbox}${proxy_remote_file}@`hostname -f`:${proxy_local_file}"
        need_to_reset_proxy=yes
    fi
fi

[ -z "$blahpd_inputsandbox" ] || echo "#PBS -W stagein=$blahpd_inputsandbox" >> $tmp_file

# Set the required environment variables (escape values with double quotes)
if [ "x$envir" != "x" ]  
then
    echo "" >> $tmp_file
    echo "# Setting the environment:" >> $tmp_file
    echo "export `echo ';'$envir | sed -e 's/;\([^=]*\)=\([^;]*\)/ \1=\"\2\"/g'`" >> $tmp_file
fi

# Set the path to the user proxy
if [ "x$need_to_reset_proxy" == "xyes" ] ; then
    echo "export X509_USER_PROXY=\`pwd\`/${proxy_remote_file}" >> $tmp_file
fi

# Add the command (with full path if not staged)
echo "" >> $tmp_file
echo "# Command to execute:" >> $tmp_file
if [ "x$stgcmd" == "xyes" ] 
then
    the_command="./`basename $the_command`"
    echo "if [ ! -x $the_command ]; then chmod u+x $the_command; fi" >> $tmp_file
fi
echo "$the_command $arguments" >> $tmp_file

# Exit if it was just a test
if [ "x$debug" == "xyes" ]
then
    exit 255
fi

# Let the wrap script be at least 1 second older than logfile
# for subsequent "find -newer" command to work
sleep 1


###############################################################
# Submit the script
###############################################################
curdir=`pwd`

if [ "x$workdir" != "x" ]; then
    cd $workdir
fi

if [ $? -ne 0 ]; then
    echo "Failed to CD to Initial Working Directory." >&2
    echo Error # for the sake of waiting fgets in blahpd
    exit 1
fi

jobID=`qsub $curdir/$tmp_file` # actual submission
retcode=$?

# Sleep for a while to allow job enter the queue
sleep 2

# find the correct logfile (it must have been modified
# *more* recently than the wrapper script)
logfile=`find $logpath -type f -newer $curdir/$tmp_file -exec grep -l "job name = $tmp_file" {} \;`

if [ -z "$logfile" ]; then
    echo "Error: job not found in logs" >&2
    echo Error # for the sake of waiting fgets in blahpd
    exit 1
fi

                                                                                                                                                             
# Don't trust qsub retcode, it could have crashed
# between submission and id output, and we would
# loose track of the job

# Search for the job in the logfile using job name
jobID_log=`grep "job name = $tmp_file" $logfile | awk -F";" '{ print $5 }'`
if [ "$jobID_log" != "$jobID" ]; then
    echo "WARNING: JobID in log file is different from the one returned by qsub!" >&2
    echo "($jobID_log != $jobID)" >&2
    echo "I'll be using the one in the log ($jobID_log)..." >&2
fi

# Compose the blahp jobID (log file + pbs jobid)
echo `basename $logfile`"/"$jobID

# Clean temporay files
cd $curdir
rm $tmp_file

exit $retcode
