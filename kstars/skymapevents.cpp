/***************************************************************************
                          skymapevents.cpp  -  K Desktop Planetarium
                             -------------------
    begin                : Sat Feb 10 2001
    copyright            : (C) 2001 by Jason Harris
    email                : jharris@30doradus.org
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

//This file contains Event handlers for the SkyMap class.

#include <qcursor.h>

#include <kiconloader.h>
#include <kstatusbar.h>
#include <kshortcut.h> //KKey class
#include <stdlib.h>
#include <math.h> //using fabs()

#include "infoboxes.h"
#include "kstars.h"
#include "skymap.h"
#include "ksutils.h"
#include "ksfilereader.h"
#include "kspopupmenu.h"

void SkyMap::resizeEvent( QResizeEvent * )
{
	computeSkymap = true; // skymap must be new computed
	if ( testWState(WState_AutoMask) ) updateMask();

	// avoid phantom positions of infoboxes
	if ( ksw && ( isVisible() || width() == ksw->width() || height() == ksw->height() ) ) {
		infoBoxes()->resize( width(), height() );
	}
	sky->resize( width(), height() );
}

void SkyMap::keyPressEvent( QKeyEvent *e ) {
	QString s;
	bool arrowKeyPressed( false );
	float step = 1.0;
	if ( e->state() & ShiftButton ) step = 2.0;

	//If the DCOP resume key was pressed, we process it here
	if ( ! data->resumeKey.isNull() && e->key() == data->resumeKey.keyCodeQt() ) {
		//kdDebug() << "resumeKey pressed; resuming DCOP." << endl;
		ksw->resumeDCOP();
		return;
	}

	switch ( e->key() ) {
		case Key_Left :
			if ( data->options->useAltAz ) {
				focus()->setAz( focus()->az()->Degrees() - step * MINZOOM/zoomFactor() );
				focus()->HorizontalToEquatorial( data->LST, data->geo()->lat() );
			} else {
				focus()->setRA( focus()->ra()->Hours() + 0.05*step * MINZOOM/zoomFactor() );
				focus()->EquatorialToHorizontal( data->LST, data->geo()->lat() );
			}

			arrowKeyPressed = true;
			slewing = true;
			++scrollCount;
			break;

		case Key_Right :
			if ( data->options->useAltAz ) {
				focus()->setAz( focus()->az()->Degrees() + step * MINZOOM/zoomFactor() );
				focus()->HorizontalToEquatorial( data->LST, data->geo()->lat() );
			} else {
				focus()->setRA( focus()->ra()->Hours() - 0.05*step * MINZOOM/zoomFactor() );
				focus()->EquatorialToHorizontal( data->LST, data->geo()->lat() );
			}

			arrowKeyPressed = true;
			slewing = true;
			++scrollCount;
			break;

		case Key_Up :
			if ( data->options->useAltAz ) {
				focus()->setAlt( focus()->alt()->Degrees() + step * MINZOOM/zoomFactor() );
				if ( focus()->alt()->Degrees() > 90.0 ) focus()->setAlt( 90.0 );
				focus()->HorizontalToEquatorial( data->LST, data->geo()->lat() );
			} else {
				focus()->setDec( focus()->dec()->Degrees() + step * MINZOOM/zoomFactor() );
				if (focus()->dec()->Degrees() > 90.0) focus()->setDec( 90.0 );
				focus()->EquatorialToHorizontal( data->LST, data->geo()->lat() );
			}

			arrowKeyPressed = true;
			slewing = true;
			++scrollCount;
			break;

		case Key_Down:
			if ( data->options->useAltAz ) {
				focus()->setAlt( focus()->alt()->Degrees() - step * MINZOOM/zoomFactor() );
				if ( focus()->alt()->Degrees() < -90.0 ) focus()->setAlt( -90.0 );
				focus()->HorizontalToEquatorial( data->LST, data->geo()->lat() );
			} else {
				focus()->setDec( focus()->dec()->Degrees() - step * MINZOOM/zoomFactor() );
				if (focus()->dec()->Degrees() < -90.0) focus()->setDec( -90.0 );
				focus()->EquatorialToHorizontal( data->LST, data->geo()->lat() );
			}

			arrowKeyPressed = true;
			slewing = true;
			++scrollCount;
			break;

		case Key_Plus:   //Zoom in
		case Key_Equal:
			if ( ksw ) ksw->slotZoomIn();
			break;

		case Key_Minus:  //Zoom out
		case Key_Underscore:
			if ( ksw ) ksw->slotZoomOut();
			break;

//In the following cases, we set slewing=true in order to disengage tracking
		case Key_N: //center on north horizon
			stopTracking();
			setDestinationAltAz( 15.0, 0.0001 );
			break;

		case Key_E: //center on east horizon
			stopTracking();
			setDestinationAltAz( 15.0, 90.0 );
			break;

		case Key_S: //center on south horizon
			stopTracking();
			setDestinationAltAz( 15.0, 180.0 );
			break;

		case Key_W: //center on west horizon
			stopTracking();
			setDestinationAltAz( 15.0, 270.0 );
			break;

		case Key_Z: //center on Zenith
			stopTracking();
			setDestinationAltAz( 90.0, focus()->az()->Degrees() );
			break;

		case Key_0: //center on Sun
			setClickedObject( data->PCat->findByName("Sun") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_1: //center on Mercury
			setClickedObject( data->PCat->findByName("Mercury") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_2: //center on Venus
			setClickedObject( data->PCat->findByName("Venus") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_3: //center on Moon
			setClickedObject( data->Moon );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_4: //center on Mars
			setClickedObject( data->PCat->findByName("Mars") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_5: //center on Jupiter
			setClickedObject( data->PCat->findByName("Jupiter") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_6: //center on Saturn
			setClickedObject( data->PCat->findByName("Saturn") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_7: //center on Uranus
			setClickedObject( data->PCat->findByName("Uranus") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_8: //center on Neptune
			setClickedObject( data->PCat->findByName("Neptune") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_9: //center on Pluto
			setClickedObject( data->PCat->findByName("Pluto") );
			setClickedPoint( clickedObject() );
			slotCenter();
			break;

		case Key_BracketLeft:   // Begin measuring angular distance
			if ( !isAngleMode() ) {
				slotBeginAngularDistance();
			}

			break;

		case Key_BracketRight:  // End measuring angular distance
			if ( isAngleMode() ) {
				slotEndAngularDistance();
			}

			break;

//DUMP_HORIZON
/*
		case Key_X: //Spit out coords of horizon polygon
			dumpHorizon = true;
			break;
*/
//END_DUMP_HORIZON

//TIMING

		case Key_T: //loop through all objects, get Sin, Cos, Rad
		{

      QFile file;
     	if ( KSUtils::openDataFile( file, "Cities.dat" ) ) {
        KSFileReader fileReader( file );
        int nCount = 0;
        while (fileReader.hasMoreLines()) {
          QString line = fileReader.readLine();
          nCount++;
    			kdDebug() << "Line " << nCount << " : " << line;
        }
      }

/*
			QTime t1;
			t1.start();
      for (int i=0;i<10;i++) {
      	if ( KSUtils::openDataFile( file, "Cities.dat" ) ) {
          QString sAll( file.readAll() );
          QStringList lines = QStringList::split( "\n", sAll );
          int nSize = lines.size();
          for ( int i=0; i<nSize; i++ ) {
            QString& line = lines[i];
          }
      		file.close();
        }
      }
			kdDebug() << "time taken for reading city data via read all (10 times): (msec): " << t1.elapsed() << endl;

			QTime t2;
			t2.start();
      for (int i=0;i<10;i++) {
      	if ( KSUtils::openDataFile( file, "Cities.dat" ) ) {
      		QTextStream stream( &file );
        	while ( !stream.eof() ) {
      			QString line = stream.readLine();
      		}
      		file.close();
      	}
      }
			kdDebug() << "time taken for reading city data old code (10 times): (msec): " << t2.elapsed() << endl;

			QTime t3;
			t3.start();
      for (int i=0;i<1;i++) {
      	if ( KSUtils::openDataFile( file, "ngcic.dat" ) ) {
          QString sAll( file.readAll() );
          QStringList lines = QStringList::split( "\n", sAll );
          int nSize = lines.size();
          for ( int i=0; i<nSize; i++ ) {
            QString& line = lines[i];
          }
      		file.close();
        }
      }
			kdDebug() << "time taken for reading deep sky data via read all (1 times): (msec): " << t3.elapsed() << endl;

			QTime t4;
			t4.start();
      for (int i=0;i<1;i++) {
      	if ( KSUtils::openDataFile( file, "ngcic.dat" ) ) {
      		QTextStream stream( &file );
        	while ( !stream.eof() ) {
      			QString line = stream.readLine();
      		}
      		file.close();
      	}
      }
			kdDebug() << "time taken for reading deep sky data old code  (1 times): (msec): " << t4.elapsed() << endl;

*/
			break;
		}

//END_TIMING
	}

	setOldFocus( focus() );
	oldfocus()->setAz( focus()->az()->Degrees() );
	oldfocus()->setAlt( focus()->alt()->Degrees() );

	double dHA = data->LST->Hours() - focus()->ra()->Hours();
	while ( dHA < 0.0 ) dHA += 24.0;
	data->HourAngle->setH( dHA );

	if ( arrowKeyPressed ) {
		infoBoxes()->focusObjChanged( i18n( "nothing" ) );
		stopTracking();
		
		if ( scrollCount > 10 ) {
			setDestination( focus() );
			scrollCount = 0;
		}
	}

	forceUpdate(); //need a total update, or slewing with the arrow keys doesn't work.
}

void SkyMap::stopTracking() {
	if ( ksw && data->options->isTracking ) ksw->slotTrack();
}

void SkyMap::keyReleaseEvent( QKeyEvent *e ) {
	switch ( e->key() ) {
		case Key_Left :  //no break; continue to Key_Down
		case Key_Right :  //no break; continue to Key_Down
		case Key_Up :  //no break; continue to Key_Down
		case Key_Down :
			slewing = false;
			scrollCount = 0;

			setDestination( focus() );
			if ( data->options->useAltAz )
				destination()->EquatorialToHorizontal( data->LST, data->options->Location()->lat() );

			showFocusCoords();
			forceUpdate();	// Need a full update to draw faint objects that are not drawn while slewing.
			break;
	}
}

void SkyMap::mouseMoveEvent( QMouseEvent *e ) {
	// Are we in angularDistanceMode: Then draw a line
	
	if ( isAngleMode() ) {

//		KStarsOptions* options = ksw->options();
//		beginRulerPoint = getXY( previousClickedPoint(), options->useAltAz, false);
		beginRulerPoint = getXY( previousClickedPoint(), false, true);
		endRulerPoint =  QPoint(e->x(), e->y());
		update();
		return;
	}
		
	
	//Are we dragging an infoBox?
	if ( infoBoxes()->dragBox( e ) ) {
		update();
		return;
	}

	//Are we defining a ZoomRect?
	if ( ZoomRect.center().x() > 0 && ZoomRect.center().y() > 0 ) {
		//cancel operation if the user let go of CTRL
		if ( !( e->state() & ControlButton ) ) {
			ZoomRect = QRect(); //invalidate ZoomRect
			update();
		} else {
			//Resize the rectangle so that it passes through the cursor position
			QPoint pcenter = ZoomRect.center();
			int dx = abs(e->x() - pcenter.x());
			int dy = abs(e->y() - pcenter.y());
			if ( dx == 0 || float(dy)/float(dx) > float(height())/float(width()) ) {
				//Size rect by height
				ZoomRect.setHeight( 2*dy );
				ZoomRect.setWidth( 2*dy*width()/height() );
			} else {
				//Size rect by height
				ZoomRect.setWidth( 2*dx );
				ZoomRect.setHeight( 2*dx*height()/width() );
			}
			ZoomRect.moveCenter( pcenter ); //reset center

			update();
			return;
		}
	}

	double dx = ( 0.5*width()  - e->x() )/zoomFactor();
	double dy = ( 0.5*height() - e->y() )/zoomFactor();
	double dyPix = 0.5*height() - e->y();

	if (unusablePoint (dx, dy)) return;	// break if point is unusable

	//determine RA, Dec of mouse pointer
	setMousePoint( dXdYToRaDec( dx, dy, data->options->useAltAz, data->LST, data->geo()->lat() ) );


	if ( midMouseButtonDown ) { //zoom according to y-offset
		float yoff = dyPix - y0;

		if (yoff > 10 ) {
			y0 = dyPix;
			if ( ksw ) ksw->slotZoomIn();
		}
		if (yoff < -10 ) {
			y0 = dyPix;
			if ( ksw ) ksw->slotZoomOut();
		}
	}

	if ( mouseButtonDown ) {
		// set the mouseMoveCursor and set slewing=true, if they are not set yet
		if (!mouseMoveCursor) setMouseMoveCursor();
		if (!slewing) {
			slewing = true;
			infoBoxes()->focusObjChanged( i18n( "nothing" ) );
			if ( ksw && data->options->isTracking ) ksw->slotTrack(); //toggle tracking off
		}

		//Update focus such that the sky coords at mouse cursor remain approximately constant
		if ( data->options->useAltAz ) {
			mousePoint()->EquatorialToHorizontal( data->LST, data->geo()->lat() );
			clickedPoint()->EquatorialToHorizontal( data->LST, data->geo()->lat() );
			dms dAz = mousePoint()->az()->Degrees() - clickedPoint()->az()->Degrees();
			dms dAlt = mousePoint()->alt()->Degrees() - clickedPoint()->alt()->Degrees();
			focus()->setAz( focus()->az()->Degrees() - dAz.Degrees() ); //move focus in opposite direction
			focus()->setAlt( focus()->alt()->Degrees() - dAlt.Degrees() );

			if ( focus()->alt()->Degrees() >90.0 ) focus()->setAlt( 89.9999 );
			if ( focus()->alt()->Degrees() <-90.0 ) focus()->setAlt( -89.9999 );
			focus()->setAz( focus()->az()->reduce() );
			focus()->HorizontalToEquatorial( data->LST, data->geo()->lat() );
		} else {
			dms dRA = mousePoint()->ra()->Degrees() - clickedPoint()->ra()->Degrees();
			dms dDec = mousePoint()->dec()->Degrees() - clickedPoint()->dec()->Degrees();
			focus()->setRA( focus()->ra()->Hours() - dRA.Hours() ); //move focus in opposite direction
			focus()->setDec( focus()->dec()->Degrees() - dDec.Degrees() );

			if ( focus()->dec()->Degrees() >90.0 ) focus()->setDec( 90.0 );
			if ( focus()->dec()->Degrees() <-90.0 ) focus()->setDec( -90.0 );
			focus()->setRA( focus()->ra()->reduce() );
			focus()->EquatorialToHorizontal( data->LST, data->geo()->lat() );
		}

		++scrollCount;
		if ( scrollCount > 4 ) {
			showFocusCoords();
			scrollCount = 0;
		}

		setOldFocus( focus() );

		double dHA = data->LST->Hours() - focus()->ra()->Hours();
		while ( dHA < 0.0 ) dHA += 24.0;
		data->HourAngle->setH( dHA );

		//redetermine RA, Dec of mouse pointer, using new focus
		setMousePoint( dXdYToRaDec( dx, dy, data->options->useAltAz, data->LST, data->geo()->lat() ) );
		setClickedPoint( mousePoint() );

		forceUpdate();  // must be new computed
	} else {

		QString sRA, sDec, s;
		sRA = mousePoint()->ra()->toHMSString();
		sDec = mousePoint()->dec()->toDMSString(true); //true = force +/- symbol
		s = sRA + ",  " + sDec;
		if ( ksw ) ksw->statusBar()->changeItem( s, 1 );
	}
}

void SkyMap::wheelEvent( QWheelEvent *e ) {
	if ( ksw && e->delta() > 0 ) ksw->slotZoomIn();
	else if ( ksw ) ksw->slotZoomOut();
}

void SkyMap::mouseReleaseEvent( QMouseEvent * ) {
	if ( infoBoxes()->unGrabBox() ) {
		update();
		return;
	}

	if ( ZoomRect.isValid() ) {
		//Zoom in on center of Zoom Circle, by a factor equal to the ratio
		//of the sky pixmap's width to the Zoom Circle's diameter
		float factor = float(width()) / float(ZoomRect.width());

		double dx = ( 0.5*width()  - ZoomRect.center().x() )/zoomFactor();
		double dy = ( 0.5*height() - ZoomRect.center().y() )/zoomFactor();

		SkyPoint newcenter = dXdYToRaDec( dx, dy, data->options->useAltAz, data->LST, data->geo()->lat() );
		setClickedPoint( &newcenter );
		setClickedObject( NULL );
		setFocusObject( NULL );
		setFocus( &newcenter );
		setDestination( &newcenter );
		setOldFocus( &newcenter );
		ksw->zoom( zoomFactor() * factor );

		ZoomRect = QRect(); //invalidate ZoomRect
		forceUpdate();
	} else {
		ZoomRect = QRect(); //just in case user Ctrl+clicked + released w/o dragging...
	}

	if (mouseMoveCursor) setDefaultMouseCursor();	// set default cursor
	if (mouseButtonDown) { //false if double-clicked, because it's unset there.
		mouseButtonDown = false;
		if ( slewing ) {
			setDestination( focus() );
			slewing = false;
		}

		setOldFocus( focus() );
		forceUpdate();	// is needed because after moving the sky not all stars are shown
	}
	if ( midMouseButtonDown ) midMouseButtonDown = false;  // if middle button was pressed unset here

	scrollCount = 0;
}

void SkyMap::mousePressEvent( QMouseEvent *e ) {
	//did we Grab an infoBox?
	if ( e->button() == LeftButton && infoBoxes()->grabBox( e ) ) {
		update(); //refresh without redrawing skymap
		return;
	}

	if ( (e->state() & ControlButton) && (e->button() == LeftButton) ) {
		ZoomRect.moveCenter( e->pos() );
		update(); //refresh without redrawing skymap
		return;
	}

	// if button is down and cursor is not moved set the move cursor after 500 ms
	QTimer t;
	t.singleShot (500, this, SLOT (setMouseMoveCursor()));

	double dx = ( 0.5*width()  - e->x() )/zoomFactor();
	double dy = ( 0.5*height() - e->y() )/zoomFactor();
	if (unusablePoint (dx, dy)) return;	// break if point is unusable

	if ( !midMouseButtonDown && e->button() == MidButton ) {
		y0 = 0.5*height() - e->y();  //record y pixel coordinate for middle-button zooming
		midMouseButtonDown = true;
	}

	if ( !mouseButtonDown ) {
		if ( e->button()==LeftButton ) {
			mouseButtonDown = true;
			scrollCount = 0;
		}

		//determine RA, Dec of mouse pointer
		setMousePoint( dXdYToRaDec( dx, dy, data->options->useAltAz,
				data->LST, data->geo()->lat(), data->options->useRefraction ) );
		setClickedPoint( mousePoint() );

		//Find object nearest to clickedPoint()
		setClickedObject( objectNearest( clickedPoint() ) );

		if ( clickedObject() ) {
			setClickedPoint( clickedObject() );
			
			if ( e->button() == RightButton ) {
				clickedObject()->showPopupMenu( pmenu, QCursor::pos() );
			}
			
			if ( ksw && e->button() == LeftButton ) {
				ksw->statusBar()->changeItem( i18n(clickedObject()->longname().utf8()), 0 );
			}
		} else {
			//Empty sky selected.  If left-click, display "nothing" in the status bar.
	    //If right-click, open "empty" popup menu.
			setClickedObject( NULL );

			switch (e->button()) {
				case LeftButton:
					if ( ksw ) ksw->statusBar()->changeItem( i18n( "Empty sky" ), 0 );
					break;
				case RightButton:
				{
					SkyObject *nullObj = new SkyObject( SkyObject::TYPE_UNKNOWN, clickedPoint()->ra()->Hours(), clickedPoint()->dec()->Degrees() );
					pmenu->createEmptyMenu( nullObj );
					delete nullObj;
					
					pmenu->popup(  QCursor::pos() );
					break;
				}
				
				default:
					break;
			}
		}
	}
}

void SkyMap::mouseDoubleClickEvent( QMouseEvent *e ) {
	//Was the event inside an infoBox?  If so, shade the box.
	if ( e->button() == LeftButton ) {
		if ( infoBoxes()->shadeBox( e ) ) {
			update();
			return;
		}

		double dx = ( 0.5*width()  - e->x() )/zoomFactor();
		double dy = ( 0.5*height() - e->y() )/zoomFactor();
		if (unusablePoint (dx, dy)) return;	// break if point is unusable

		if (mouseButtonDown ) mouseButtonDown = false;
		if ( dx != 0.0 || dy != 0.0 )  slotCenter();
	}
}

void SkyMap::paintEvent( QPaintEvent * )
{
	KStarsOptions* options = data->options;

// if the skymap should be only repainted and constellations need not to be new computed; call this with update() (default)
	if (!computeSkymap)
	{
		QPixmap *sky2 = new QPixmap( *sky );
		drawOverlays( sky2 );
		bitBlt( this, 0, 0, sky2 );
		delete sky2;
		return ; // exit because the pixmap is repainted and that's all what we want
	}

// if the sky should be recomputed (this is not every paintEvent call needed, explicitly call with forceUpdate())
	QPainter psky;

	setMapGeometry();

//checkSlewing combines the slewing flag (which is true when the display is actually in motion),
//the hideOnSlew option (which is true if slewing should hide objects),
//and clockSlewing (which is true if the timescale exceeds options()->slewTimeScale)
	bool checkSlewing = ( ( slewing || ( clockSlewing && data->clock()->isActive() ) )
				&& options->hideOnSlew );

//shortcuts to inform wheter to draw different objects
	bool drawPlanets( options->drawPlanets && !(checkSlewing && options->hidePlanets) );
	bool drawMW( options->drawMilkyWay && !(checkSlewing && options->hideMW) );
	bool drawCNames( options->drawConstellNames && !(checkSlewing && options->hideCNames) );
	bool drawCLines( options->drawConstellLines &&!(checkSlewing && options->hideCLines) );
	bool drawCBounds( options->drawConstellBounds &&!(checkSlewing && options->hideCBounds) );
	bool drawGrid( options->drawGrid && !(checkSlewing && options->hideGrid) );

	psky.begin( sky );
	psky.fillRect( 0, 0, width(), height(), QBrush( options->colorScheme()->colorNamed( "SkyColor" ) ) );

	QFont stdFont = psky.font();
	QFont smallFont = psky.font();
	smallFont.setPointSize( stdFont.pointSize() - 2 );

	if ( drawMW ) drawMilkyWay( psky );
	if ( drawGrid ) drawCoordinateGrid( psky );
	if ( data->options->drawEquator ) drawEquator( psky );
	if ( options->drawEcliptic ) drawEcliptic( psky );
	
	if ( drawCBounds ) drawConstellationBoundaries( psky );
	if ( drawCLines ) drawConstellationLines( psky );
	if ( drawCNames ) drawConstellationNames( psky, stdFont );

	// stars and planets use the same font size
	if ( zoomFactor() < 10.*MINZOOM ) {
		psky.setFont( smallFont );
	} else {
		psky.setFont( stdFont );
	}

	//drawing to screen, so leave scale parameter at its default value of 1.0
	drawStars( psky );
	drawDeepSkyObjects( psky );
	drawSolarSystem( psky, drawPlanets );
	drawAttachedLabels( psky );
	drawHorizon( psky, stdFont );

	//Finish up
	psky.end();

	QPixmap *sky2 = new QPixmap( *sky );
	drawOverlays( sky2 );
	bitBlt( this, 0, 0, sky2 );
	delete sky2;

	computeSkymap = false;	// use forceUpdate() to compute new skymap else old pixmap will be shown
}

