# -*-mode: tcl; fill-column: 75; tab-width: 8; coding: iso-latin-1-unix -*-
#
#	$Id: STList3.tcl,v 1.2.2.1 2001/11/04 05:10:08 idiscovery Exp $
#
# Tix Demostration Program
#
# This sample program is structured in such a way so that it can be
# executed from the Tix demo program "widget": it must have a
# procedure called "RunSample". It should also have the "if" statment
# at the end of this file so that it can be run as a standalone
# program using tixwish.

# Demonstrates the use of DirTree with the TList 
#

proc RunSample {w} {
    set top [frame $w.f -bd 1 -relief raised]
    set box [tixButtonBox $w.b -bd 1 -relief raised]

    pack $box -side bottom -fill both
    pack $top -side top -fill both -expand yes

    # Create the Paned Window to contain the dirtree and scrolled tlist
    #
    set p [tixPanedWindow $top.p -orient horizontal]
    pack $p -expand yes -fill both -padx 4 -pady 4

    set p1 [$p add pane1 -expand 1]
    set p2 [$p add pane2 -expand 4]

    $p1 config -relief flat
    $p2 config -relief flat

    # Create a DirTree
    #
    tixDirTree $p1.dirtree -options {
	hlist.width 28
    }

    pack $p1.dirtree -expand yes -fill both -padx 4 -pady 4


    # Create a TList
    # NOTE: we set the width of the tlist to 60 characters, since we'll have
    #       quite a few files to display
    #
    tixScrolledTList $p2.st -options {
	tlist.orient vertical
	tlist.selectMode single
	tlist.width 60
	tlist.height 25
    }
    pack $p2.st -expand yes -fill both -padx 4 -pady 4

    set tlist [$p2.st subwidget tlist]

    # setup the callbacks: when the user selects a directory, we'll display
    # its content in the tlist widget
    $p1.dirtree config \
	-browsecmd "TList:listdir $tlist" \
	-command "TList:listdir $tlist"

    # List the directory now
    #
    TList:listdir $tlist [pwd]

    # Create the buttons
    #
    $box add ok     -text Ok     -command "destroy $w" -width 6
    $box add cancel -text Cancel -command "destroy $w" -width 6
}

proc TList:listdir {w dir} {
    $w delete 0 end

    set appPWD [pwd]

    if [catch {cd $dir} err] {
	# The user has entered an invalid directory
	# %% todo: prompt error, go back to last succeed directory
	cd $appPWD
	return
    }

    foreach fname [lsort [glob -nocomplain *]] {
	if [file isdirectory $fname] {
	    set image [tix getimage folder]
	} else {
	    continue
	}

	$w insert end -itemtype imagetext \
	    -text $fname -image $image
    }

    foreach fname [lsort [glob -nocomplain *]] {
	if [file isdirectory $fname] {
	    continue
	} elseif [string match *.c $fname] {
	    set image [tix getimage srcfile]
	} elseif [string match *.h $fname] {
	    set image [tix getimage srcfile]
	} elseif [string match *.tcl $fname] {
	    set image [tix getimage file]
	} elseif [string match *.o $fname] {
	    set image [tix getimage file]
	} else {
	    set image [tix getimage textfile]
	}

	$w insert end -itemtype imagetext \
	    -text $fname -image $image
    }

    cd $appPWD
}


if {![info exists tix_demo_running]} {
    wm withdraw .
    set w .demo
    toplevel $w; wm transient $w ""
    RunSample $w
    bind $w <Destroy> exit
}
