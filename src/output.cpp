#include "output.h"

#include "cata_utility.h"
#include "catacharset.h"
#include "color.h"
#include "cursesdef.h"
#include "input.h"
#include "item.h"
#include "line.h"
#include "name.h"
#include "options.h"
#include "popup.h"
#include "rng.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "units.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if (defined TILES || defined _WIN32 || defined WINDOWS)
#include "cursesport.h"
#endif
#ifdef __ANDROID__
#include <SDL_keyboard.h>
#endif

// Display data
int TERMX;
int TERMY;
int POSX;
int POSY;
int VIEW_OFFSET_X;
int VIEW_OFFSET_Y;
int TERRAIN_WINDOW_WIDTH;
int TERRAIN_WINDOW_HEIGHT;
int TERRAIN_WINDOW_TERM_WIDTH;
int TERRAIN_WINDOW_TERM_HEIGHT;
int FULL_SCREEN_WIDTH;
int FULL_SCREEN_HEIGHT;

int OVERMAP_WINDOW_HEIGHT;
int OVERMAP_WINDOW_WIDTH;

static std::string rm_prefix( std::string str, char c1 = '<', char c2 = '>' );

scrollingcombattext SCT;
extern bool tile_iso;
extern bool use_tiles;

extern bool test_mode;

// utf8 version
std::vector<std::string> foldstring( std::string str, int width, const char split )
{
    std::vector<std::string> lines;
    if( width < 1 ) {
        lines.push_back( str );
        return lines;
    }
    std::stringstream sstr( str );
    std::string strline;
    std::vector<std::string> tags;
    while( std::getline( sstr, strline, '\n' ) ) {
        if( strline.empty() ) {
            // Special case empty lines as std::getline() sets failbit immediately
            // if the line is empty.
            lines.emplace_back();
        } else {
            std::string wrapped = word_rewrap( strline, width, split );
            std::stringstream swrapped( wrapped );
            std::string wline;
            while( std::getline( swrapped, wline, '\n' ) ) {
                // Ensure that each line is independently color-tagged
                // Re-add tags closed in the previous line
                const std::string rawwline = wline;
                if( !tags.empty() ) {
                    std::stringstream swline;
                    for( const std::string &tag : tags ) {
                        swline << tag;
                    }
                    swline << wline;
                    wline = swline.str();
                }
                // Process the additional tags in the current line
                const std::vector<size_t> tags_pos = get_tag_positions( rawwline );
                for( const size_t tag_pos : tags_pos ) {
                    if( tag_pos + 1 < rawwline.size() && rawwline[tag_pos + 1] == '/' ) {
                        if( !tags.empty() ) {
                            tags.pop_back();
                        }
                    } else {
                        auto tag_end = rawwline.find( '>', tag_pos );
                        if( tag_end != std::string::npos ) {
                            tags.emplace_back( rawwline.substr( tag_pos, tag_end + 1 - tag_pos ) );
                        }
                    }
                }
                // Close any unclosed tags
                if( !tags.empty() ) {
                    std::stringstream swline;
                    swline << wline;
                    for( auto it = tags.rbegin(); it != tags.rend(); ++it ) {
                        // currently the only closing tag is </color>
                        swline << "</color>";
                    }
                    wline = swline.str();
                }
                // The resulting line can be printed independently and have the correct color
                lines.emplace_back( wline );
            }
        }
    }
    return lines;
}

std::string tag_colored_string( const std::string &s, nc_color color )
{
    // @todo: Make this tag generation a function, put it in good place
    std::string color_tag_open = "<color_" + string_from_color( color ) + ">";
    std::string color_tag_close = "</color>";
    return color_tag_open + s + color_tag_close;
}

std::vector<std::string> split_by_color( const std::string &s )
{
    std::vector<std::string> ret;
    std::vector<size_t> tag_positions = get_tag_positions( s );
    size_t last_pos = 0;
    std::vector<size_t>::iterator it;
    for( it = tag_positions.begin(); it != tag_positions.end(); ++it ) {
        ret.push_back( s.substr( last_pos, *it - last_pos ) );
        last_pos = *it;
    }
    // and the last (or only) one
    ret.push_back( s.substr( last_pos, std::string::npos ) );
    return ret;
}

std::string remove_color_tags( const std::string &s )
{
    std::string ret;
    std::vector<size_t> tag_positions = get_tag_positions( s );
    size_t next_pos = 0;

    if( tag_positions.size() > 1 ) {
        for( size_t i = 0; i < tag_positions.size(); ++i ) {
            ret += s.substr( next_pos, tag_positions[i] - next_pos );
            next_pos = s.find( ">", tag_positions[i], 1 ) + 1;
        }

        ret += s.substr( next_pos, std::string::npos );
    } else {
        return s;
    }
    return ret;
}

void print_colored_text( const catacurses::window &w, int y, int x, nc_color &color,
                         nc_color base_color, const std::string &text )
{
    if( y > -1 && x > -1 ) {
        wmove( w, y, x );
    }
    const auto color_segments = split_by_color( text );
    for( auto seg : color_segments ) {
        if( seg.empty() ) {
            continue;
        }

        if( seg[0] == '<' ) {
            color = get_color_from_tag( seg, base_color );
            seg = rm_prefix( seg );
        }

        wprintz( w, color, seg );
    }
}

void trim_and_print( const catacurses::window &w, int begin_y, int begin_x, int width,
                     nc_color base_color, const std::string &text )
{
    std::string sText;
    if( utf8_width( remove_color_tags( text ) ) > width ) {

        int iLength = 0;
        std::string sTempText;
        std::string sColor;

        const auto color_segments = split_by_color( text );
        for( const std::string &seg : color_segments ) {
            sColor.clear();

            if( !seg.empty() && ( seg.substr( 0, 7 ) == "<color_" || seg.substr( 0, 7 ) == "</color" ) ) {
                sTempText = rm_prefix( seg );

                if( seg.substr( 0, 7 ) == "<color_" ) {
                    sColor = seg.substr( 0, seg.find( '>' ) + 1 );
                }
            } else {
                sTempText = seg;
            }

            const int iTempLen = utf8_width( sTempText );
            iLength += iTempLen;

            if( iLength > width ) {
                sTempText = sTempText.substr( 0, cursorx_to_position( sTempText.c_str(),
                                              iTempLen - ( iLength - width ) - 1, NULL, -1 ) ) + "\u2026";
            }

            sText += sColor + sTempText;
            if( !sColor.empty() ) {
                sText += "</color>";
            }

            if( iLength > width ) {
                break;
            }
        }
    } else {
        sText = text;
    }

    print_colored_text( w, begin_y, begin_x, base_color, base_color, sText );
}

int print_scrollable( const catacurses::window &w, int begin_line, const std::string &text,
                      nc_color base_color, const std::string &scroll_msg )
{
    const size_t wwidth = getmaxx( w );
    const auto text_lines = foldstring( text, wwidth );
    size_t wheight = getmaxy( w );
    const auto print_scroll_msg = text_lines.size() > wheight;
    if( print_scroll_msg && !scroll_msg.empty() ) {
        // keep the last line free for a message to the player
        wheight--;
    }
    if( begin_line < 0 || text_lines.size() <= wheight ) {
        begin_line = 0;
    } else if( begin_line + wheight >= text_lines.size() ) {
        begin_line = text_lines.size() - wheight;
    }
    nc_color color = base_color;
    for( size_t i = 0; i + begin_line < text_lines.size() && i < wheight; ++i ) {
        print_colored_text( w, i, 0, color, base_color, text_lines[i + begin_line] );
    }
    if( print_scroll_msg && !scroll_msg.empty() ) {
        color = c_white;
        print_colored_text( w, wheight, 0, color, color, scroll_msg );
    }
    return std::max<int>( 0, text_lines.size() - wheight );
}

// returns number of printed lines
int fold_and_print( const catacurses::window &w, int begin_y, int begin_x, int width,
                    nc_color base_color, const std::string &text, const char split )
{
    nc_color color = base_color;
    std::vector<std::string> textformatted;
    textformatted = foldstring( text, width, split );
    for( int line_num = 0; static_cast<size_t>( line_num ) < textformatted.size(); line_num++ ) {
        print_colored_text( w, line_num + begin_y, begin_x, color, base_color, textformatted[line_num] );
    }
    return textformatted.size();
}

int fold_and_print_from( const catacurses::window &w, int begin_y, int begin_x, int width,
                         int begin_line, nc_color base_color, const std::string &text )
{
    const int iWinHeight = getmaxy( w );
    nc_color color = base_color;
    std::vector<std::string> textformatted;
    textformatted = foldstring( text, width );
    for( int line_num = 0; static_cast<size_t>( line_num ) < textformatted.size(); line_num++ ) {
        if( line_num + begin_y - begin_line == iWinHeight ) {
            break;
        }
        if( line_num >= begin_line ) {
            wmove( w, line_num + begin_y - begin_line, begin_x );
        }
        // split into colorable sections
        std::vector<std::string> color_segments = split_by_color( textformatted[line_num] );
        // for each section, get the color, and print it
        std::vector<std::string>::iterator it;
        for( it = color_segments.begin(); it != color_segments.end(); ++it ) {
            if( !it->empty() && it->at( 0 ) == '<' ) {
                color = get_color_from_tag( *it, base_color );
            }
            if( line_num >= begin_line ) {
                std::string l = rm_prefix( *it );
                if( l != "--" ) { // -- is a separation line!
                    wprintz( w, color, rm_prefix( *it ) );
                } else {
                    for( int i = 0; i < width; i++ ) {
                        wputch( w, c_dark_gray, LINE_OXOX );
                    }
                }
            }
        }
    }
    return textformatted.size();
}

void multipage( const catacurses::window &w, const std::vector<std::string> &text,
                const std::string &caption,
                int begin_y )
{
    int height = getmaxy( w );
    int width = getmaxx( w );

    //Do not erase the current screen if it's not first line of the text
    if( begin_y == 0 ) {
        werase( w );
    }

    /* TODO:
        issue:     # of lines in the paragraph > height -> inf. loop;
        solution:  split this paragraph in two pieces;
    */
    for( int i = 0; i < static_cast<int>( text.size() ); i++ ) {
        if( begin_y == 0 && !caption.empty() ) {
            begin_y = fold_and_print( w, 0, 1, width - 2, c_white, caption ) + 1;
        }
        std::vector<std::string> next_paragraph = foldstring( text[i], width - 2 );
        if( begin_y + static_cast<int>( next_paragraph.size() ) > height - ( ( i + 1 ) < static_cast<int>
                ( text.size() ) ? 1 : 0 ) ) {
            // Next page
            i--;
            center_print( w, height - 1, c_light_gray, _( "Press any key for more..." ) );
            wrefresh( w );
            catacurses::refresh();
            inp_mngr.wait_for_any_key();
            werase( w );
            begin_y = 0;
        } else {
            begin_y += fold_and_print( w, begin_y, 1, width - 2, c_white, text[i] ) + 1;
        }
    }
    wrefresh( w );
    catacurses::refresh();
    inp_mngr.wait_for_any_key();
}

// returns single string with left aligned name and right aligned value
std::string name_and_value( const std::string &name, const std::string &value, int field_width )
{
    int name_width = utf8_width( name );
    int value_width = utf8_width( value );
    std::stringstream result;
    result << name.c_str();
    for( int i = ( name_width + value_width );
         i < std::max( field_width, name_width + value_width ); ++i ) {
        result << " ";
    }
    result << value.c_str();
    return result.str();
}

std::string name_and_value( const std::string &name, int value, int field_width )
{
    return name_and_value( name, string_format( "%d", value ), field_width );
}

void center_print( const catacurses::window &w, const int y, const nc_color FG,
                   const std::string &text )
{
    int window_width = getmaxx( w );
    int string_width = utf8_width( text );
    int x;
    if( string_width >= window_width ) {
        x = 0;
    } else {
        x = ( window_width - string_width ) / 2;
    }
    mvwprintz( w, y, x, FG, text );
}

int right_print( const catacurses::window &w, const int line, const int right_indent,
                 const nc_color FG, const std::string &text )
{
    const int available_width = std::max( 1, getmaxx( w ) - right_indent );
    const int x = std::max( 0, available_width - utf8_width( text, true ) );
    trim_and_print( w, line, x, available_width, FG, text );
    return x;
}

void wputch( const catacurses::window &w, nc_color FG, long ch )
{
    wattron( w, FG );
    waddch( w, ch );
    wattroff( w, FG );
}

void mvwputch( const catacurses::window &w, int y, int x, nc_color FG, long ch )
{
    wattron( w, FG );
    mvwaddch( w, y, x, ch );
    wattroff( w, FG );
}

void mvwputch( const catacurses::window &w, int y, int x, nc_color FG, const std::string &ch )
{
    wattron( w, FG );
    mvwprintw( w, y, x, ch );
    wattroff( w, FG );
}

void mvwputch_inv( const catacurses::window &w, int y, int x, nc_color FG, long ch )
{
    nc_color HC = invert_color( FG );
    wattron( w, HC );
    mvwaddch( w, y, x, ch );
    wattroff( w, HC );
}

void mvwputch_inv( const catacurses::window &w, int y, int x, nc_color FG, const std::string &ch )
{
    nc_color HC = invert_color( FG );
    wattron( w, HC );
    mvwprintw( w, y, x, ch );
    wattroff( w, HC );
}

void mvwputch_hi( const catacurses::window &w, int y, int x, nc_color FG, long ch )
{
    nc_color HC = hilite( FG );
    wattron( w, HC );
    mvwaddch( w, y, x, ch );
    wattroff( w, HC );
}

void mvwputch_hi( const catacurses::window &w, int y, int x, nc_color FG, const std::string &ch )
{
    nc_color HC = hilite( FG );
    wattron( w, HC );
    mvwprintw( w, y, x, ch );
    wattroff( w, HC );
}

void draw_custom_border( const catacurses::window &w, const catacurses::chtype ls,
                         const catacurses::chtype rs, const catacurses::chtype ts, const catacurses::chtype bs,
                         const catacurses::chtype tl, const catacurses::chtype tr, const catacurses::chtype bl,
                         const catacurses::chtype br, const nc_color FG, const int posy, int height, const int posx,
                         int width )
{
    wattron( w, FG );

    height = ( height == 0 ) ? getmaxy( w ) - posy : height;
    width = ( width == 0 ) ? getmaxx( w ) - posx : width;

    for( int j = posy; j < height + posy - 1; j++ ) {
        if( ls > 0 ) {
            mvwputch( w, j, posx, c_light_gray, ( ls > 1 ) ? ls : LINE_XOXO ); // |
        }

        if( rs > 0 ) {
            mvwputch( w, j, posx + width - 1, c_light_gray, ( rs > 1 ) ? rs : LINE_XOXO ); // |
        }
    }

    for( int j = posx; j < width + posx - 1; j++ ) {
        if( ts > 0 ) {
            mvwputch( w, posy, j, c_light_gray, ( ts > 1 ) ? ts : LINE_OXOX ); // --
        }

        if( bs > 0 ) {
            mvwputch( w, posy + height - 1, j, c_light_gray, ( bs > 1 ) ? bs : LINE_OXOX ); // --
        }
    }

    if( tl > 0 ) {
        mvwputch( w, posy, posx, c_light_gray, ( tl > 1 ) ? tl : LINE_OXXO ); // |^
    }

    if( tr > 0 ) {
        mvwputch( w, posy, posx + width - 1, c_light_gray, ( tr > 1 ) ? tr : LINE_OOXX ); // ^|
    }

    if( bl > 0 ) {
        mvwputch( w, posy + height - 1, posx + 0, c_light_gray, ( bl > 1 ) ? bl : LINE_XXOO ); // |_
    }

    if( br > 0 ) {
        mvwputch( w, posy + height - 1, posx + width - 1, c_light_gray, ( br > 1 ) ? br : LINE_XOOX ); // _|
    }

    wattroff( w, FG );
}

void draw_border( const catacurses::window &w, nc_color border_color, const std::string &title,
                  nc_color title_color )
{
    wattron( w, border_color );
    wborder( w, LINE_XOXO, LINE_XOXO, LINE_OXOX, LINE_OXOX,
             LINE_OXXO, LINE_OOXX, LINE_XXOO, LINE_XOOX );
    wattroff( w, border_color );
    if( !title.empty() ) {
        center_print( w, 0, title_color, title );
    }
}

void draw_tabs( const catacurses::window &w, int active_tab, ... )
{
    int win_width;
    win_width = getmaxx( w );
    std::vector<std::string> labels;
    va_list ap;
    va_start( ap, active_tab );
    while( char const *const tmp = va_arg( ap, char * ) ) {
        labels.push_back( tmp );
    }
    va_end( ap );

    // Draw the line under the tabs
    for( int x = 0; x < win_width; x++ ) {
        mvwputch( w, 2, x, c_white, LINE_OXOX );
    }

    int total_width = 0;
    for( auto &i : labels ) {
        total_width += i.length() + 6;    // "< |four| >"
    }

    if( total_width > win_width ) {
        //debugmsg("draw_tabs not given enough space! %s", labels[0]);
        return;
    }

    // Extra "buffer" space per each side of each tab
    double buffer_extra = ( win_width - total_width ) / ( labels.size() * 2 );
    int buffer = int( buffer_extra );
    // Set buffer_extra to (0, 1); the "extra" whitespace that builds up
    buffer_extra = buffer_extra - buffer;
    int xpos = 0;
    double savings = 0;

    for( size_t i = 0; i < labels.size(); i++ ) {
        int length = labels[i].length();
        xpos += buffer + 2;
        savings += buffer_extra;
        if( savings > 1 ) {
            savings--;
            xpos++;
        }
        mvwputch( w, 0, xpos, c_white, LINE_OXXO );
        mvwputch( w, 1, xpos, c_white, LINE_XOXO );
        mvwputch( w, 0, xpos + length + 1, c_white, LINE_OOXX );
        mvwputch( w, 1, xpos + length + 1, c_white, LINE_XOXO );
        if( static_cast<int>( i ) == active_tab ) {
            mvwputch( w, 1, xpos - 2, h_white, '<' );
            mvwputch( w, 1, xpos + length + 3, h_white, '>' );
            mvwputch( w, 2, xpos, c_white, LINE_XOOX );
            mvwputch( w, 2, xpos + length + 1, c_white, LINE_XXOO );
            mvwprintz( w, 1, xpos + 1, h_white, labels[i] );
            for( int x = xpos + 1; x <= xpos + length; x++ ) {
                mvwputch( w, 0, x, c_white, LINE_OXOX );
                mvwputch( w, 2, x, c_black, 'x' );
            }
        } else {
            mvwputch( w, 2, xpos, c_white, LINE_XXOX );
            mvwputch( w, 2, xpos + length + 1, c_white, LINE_XXOX );
            mvwprintz( w, 1, xpos + 1, c_white, labels[i] );
            for( int x = xpos + 1; x <= xpos + length; x++ ) {
                mvwputch( w, 0, x, c_white, LINE_OXOX );
            }
        }
        xpos += length + 1 + buffer;
    }
}

bool query_yn( const std::string &text )
{
    bool const force_uc = get_option<bool>( "FORCE_CAPITAL_YN" );

    const auto allow_key = [force_uc]( const input_event & evt ) {
        return !force_uc || evt.type != CATA_INPUT_KEYBOARD ||
               // std::lower is undefined outside unsigned char range
               evt.get_first_input() < 'a' || evt.get_first_input() > 'z';
    };

    return query_popup()
           .context( "YESNO" )
           .message( force_uc ?
                     pgettext( "query_yn", "<color_light_red>%s (Case Sensitive)</color>" ) :
                     pgettext( "query_yn", "<color_light_red>%s</color>" ), text )
           .option( "YES", allow_key )
           .option( "NO", allow_key )
           .cursor( 1 )
           .query()
           .action == "YES";
}

bool query_int( int &result, const std::string &text )
{
    string_input_popup popup;
    popup.title( text );
    popup.text( "" ).only_digits( true );
    popup.query();
    if( popup.canceled() ) {
        return false;
    }
    result = atoi( popup.text().c_str() );
    return true;
}

std::vector<std::string> get_hotkeys( const std::string &s )
{
    std::vector<std::string> hotkeys;
    size_t start = s.find_first_of( '<' );
    size_t end = s.find_first_of( '>' );
    if( start != std::string::npos && end != std::string::npos ) {
        // hotkeys separated by '|' inside '<' and '>', for example "<e|E|?>"
        size_t lastsep = start;
        size_t sep = s.find_first_of( '|', start );
        while( sep < end ) {
            hotkeys.push_back( s.substr( lastsep + 1, sep - lastsep - 1 ) );
            lastsep = sep;
            sep = s.find_first_of( '|', sep + 1 );
        }
        hotkeys.push_back( s.substr( lastsep + 1, end - lastsep - 1 ) );
    }
    return hotkeys;
}

long popup( const std::string &text, PopupFlags flags )
{
    query_popup pop;
    pop.message( "%s", text );
    if( flags & PF_GET_KEY ) {
        pop.allow_anykey( true );
    } else {
        pop.allow_cancel( true );
    }

    if( flags & PF_FULLSCREEN ) {
        pop.full_screen( true );
    } else if( flags & PF_ON_TOP ) {
        pop.on_top( true );
    }

    if( flags & PF_NO_WAIT ) {
        pop.show();
        return UNKNOWN_UNICODE;
    } else {
        pop.context( "POPUP_WAIT" );
        const auto &res = pop.query();
        if( res.evt.type == CATA_INPUT_KEYBOARD ) {
            return res.evt.get_first_input();
        } else {
            return UNKNOWN_UNICODE;
        }
    }
}

void popup_status( const char *const title, const std::string &fmt )
{
    std::string text;
    if( !test_mode && title != nullptr ) {
        text += title;
        text += "\n";
    }

    popup( text + fmt, PF_NO_WAIT );
}

//note that passing in iteminfo instances with sType == "DESCRIPTION" does special things
//all this should probably be cleaned up at some point, rather than using a function for things it wasn't meant for
// well frack, half the game uses it so: optional (int)selected argument causes entry highlight, and enter to return entry's key. Also it now returns int
//@param without_getch don't wait getch, return = (int)' ';
input_event draw_item_info( const int iLeft, const int iWidth, const int iTop, const int iHeight,
                            const std::string &sItemName, const std::string &sTypeName,
                            std::vector<iteminfo> &vItemDisplay, std::vector<iteminfo> &vItemCompare,
                            int &selected, const bool without_getch, const bool without_border,
                            const bool handle_scrolling, const bool scrollbar_left, const bool use_full_win,
                            const unsigned int padding )
{
    catacurses::window win = catacurses::newwin( iHeight, iWidth, iTop + VIEW_OFFSET_Y,
                             iLeft + VIEW_OFFSET_X );

#ifdef TILES
    clear_window_area( win );
#endif // TILES
    wclear( win );
    wrefresh( win );

    const auto result = draw_item_info( win, sItemName, sTypeName, vItemDisplay, vItemCompare,
                                        selected, without_getch, without_border, handle_scrolling, scrollbar_left, use_full_win,
                                        padding );
    return result;
}

std::string string_replace( std::string text, const std::string &before, const std::string &after )
{
    // Check if there's something to replace (mandatory) and it's necessary (optional)
    // Second condition assumes that text is much longer than both &before and &after.
    if( before.length() == 0 || !before.compare( after ) ) {
        return text;
    }

    const size_t before_len = before.length();
    const size_t after_len = after.length();
    size_t pos = 0;

    while( ( pos = text.find( before, pos ) ) != std::string::npos ) {
        text.replace( pos, before_len, after );
        pos += after_len;
    }

    return text;
}

std::string replace_colors( std::string text )
{
    static const std::vector<std::pair<std::string, std::string>> info_colors = {
        {"info", get_all_colors().get_name( c_cyan )},
        {"stat", get_all_colors().get_name( c_light_blue )},
        {"header", get_all_colors().get_name( c_magenta )},
        {"bold", get_all_colors().get_name( c_white )},
        {"dark", get_all_colors().get_name( c_dark_gray )},
        {"good", get_all_colors().get_name( c_green )},
        {"bad", get_all_colors().get_name( c_red )},
        {"neutral", get_all_colors().get_name( c_yellow )}
    };

    for( auto &elem : info_colors ) {
        text = string_replace( text, "<" + elem.first + ">", "<color_" + elem.second + ">" );
        text = string_replace( text, "</" + elem.first + ">", "</color>" );
    }

    return text;
}

void draw_item_filter_rules( const catacurses::window &win, int starty, int height,
                             item_filter_type type )
{
    // Clear every row, but the leftmost/rightmost pixels intact.
    const int len = getmaxx( win ) - 2;
    for( int i = 0; i < height; i++ ) {
        mvwprintz( win, starty + i, 1, c_black, std::string( len, ' ' ).c_str() );
    }

    // Not static so that language changes are correctly handled
    const std::array<std::string, 3> intros = {{
            _( "Type part of an item's name to filter it." ),
            _( "Type part of an item's name to move nearby items to the bottom." ),
            _( "Type part of an item's name to move nearby items to the top." )
        }
    };
    const int tab_idx = int( type ) - int( item_filter_type::FIRST );
    starty += 1 + fold_and_print( win, starty, 1, len, c_white, intros[tab_idx] );

    starty += fold_and_print( win, starty, 1, len, c_white, _( "Separate multiple items with ," ) );
    //~ An example of how to separate multiple items with a comma when filtering items.
    starty += 1 + fold_and_print( win, starty, 1, len, c_white, _( "Example: back,flash,aid, ,band" ) );

    if( type == item_filter_type::FILTER ) {
        starty += fold_and_print( win, starty, 1, len, c_white,
                                  _( "To exclude items, place - in front." ) );
        //~ An example of how to exclude items with - when filtering items.
        starty += 1 + fold_and_print( win, starty, 1, len, c_white, _( "Example: -pipe,-chunk,-steel" ) );
    }

    starty += fold_and_print( win, starty, 1, len, c_white,
                              _( "Search [c]ategory, [m]aterial, or [q]uality:" ) );
    //~ An example of how to filter items based on category or material.
    fold_and_print( win, starty, 1, len, c_white, _( "Example: c:food,m:iron,q:hammering" ) );
    wrefresh( win );
}

std::string format_item_info( const std::vector<iteminfo> &vItemDisplay,
                              const std::vector<iteminfo> &vItemCompare )
{
    std::ostringstream buffer;
    bool bIsNewLine = true;

    for( size_t i = 0; i < vItemDisplay.size(); i++ ) {
        if( vItemDisplay[i].sType == "DESCRIPTION" ) {
            // Always start a new line for sType == "DESCRIPTION"
            if( !bIsNewLine ) {
                buffer << "\n";
            }
            if( vItemDisplay[i].bDrawName ) {
                buffer << vItemDisplay[i].sName;
            }
            // Always end with a linebreak for sType == "DESCRIPTION"
            buffer << "\n";
            bIsNewLine = true;
        } else {
            if( vItemDisplay[i].bDrawName ) {
                buffer << vItemDisplay[i].sName;
            }

            std::string sFmt = vItemDisplay[i].sFmt;
            std::string sPost;

            //A bit tricky, find %d and split the string
            size_t pos = sFmt.find( "<num>" );
            if( pos != std::string::npos ) {
                buffer << sFmt.substr( 0, pos );
                sPost = sFmt.substr( pos + 5 );
            } else {
                buffer << sFmt;
            }

            if( vItemDisplay[i].sValue != "-999" ) {
                nc_color thisColor = c_yellow;
                for( auto &k : vItemCompare ) {
                    if( k.sValue != "-999" ) {
                        if( vItemDisplay[i].sName == k.sName && vItemDisplay[i].sType == k.sType ) {
                            if( vItemDisplay[i].dValue > k.dValue - .1 &&
                                vItemDisplay[i].dValue < k.dValue + .1 ) {
                                thisColor = c_light_gray;
                            } else if( vItemDisplay[i].dValue > k.dValue ) {
                                if( vItemDisplay[i].bLowerIsBetter ) {
                                    thisColor = c_light_red;
                                } else {
                                    thisColor = c_light_green;
                                }
                            } else if( vItemDisplay[i].dValue < k.dValue ) {
                                if( vItemDisplay[i].bLowerIsBetter ) {
                                    thisColor = c_light_green;
                                } else {
                                    thisColor = c_light_red;
                                }
                            }
                            break;
                        }
                    }
                }
                buffer << "<color_" << string_from_color( thisColor ) << ">"
                       << vItemDisplay[i].sValue
                       << "</color>";
            }
            buffer << sPost;

            // Set bIsNewLine in case the next line should always start in a new line
            if( ( bIsNewLine = vItemDisplay[i].bNewLine ) ) {
                buffer << "\n";
            }
        }
    }

    return buffer.str();
}

input_event draw_item_info( const catacurses::window &win, const std::string &sItemName,
                            const std::string &sTypeName,
                            std::vector<iteminfo> &vItemDisplay, std::vector<iteminfo> &vItemCompare,
                            int &selected, const bool without_getch, const bool without_border,
                            const bool handle_scrolling, const bool scrollbar_left, const bool use_full_win,
                            const unsigned int padding )
{
    std::ostringstream buffer;
    int line_num = use_full_win || without_border ? 0 : 1;
    if( !sItemName.empty() ) {
        buffer << sItemName << "\n";
    }
    if( sItemName != sTypeName && !sTypeName.empty() ) {
        buffer << sTypeName << "\n";
    }
    for( unsigned int i = 0; i < padding; i++ ) {
        buffer << "\n";
    }

    buffer << format_item_info( vItemDisplay, vItemCompare );

    const auto b = use_full_win ? 0 : ( without_border ? 1 : 2 );
    const auto width = getmaxx( win ) - ( use_full_win ? 1 : b * 2 );
    const auto height = getmaxy( win ) - ( use_full_win ? 0 : 2 );

    input_event result;
    while( true ) {
        int iLines = 0;
        if( !buffer.str().empty() ) {
            const auto vFolded = foldstring( buffer.str(), width - 1 );
            iLines = vFolded.size();

            if( selected < 0 ) {
                selected = 0;
            } else if( iLines < height ) {
                selected = 0;
            } else if( selected >= iLines - height ) {
                selected = iLines - height;
            }

            fold_and_print_from( win, line_num, b, width - 1, selected, c_light_gray, buffer.str() );

            draw_scrollbar( win, selected, height, iLines, ( without_border && use_full_win ? 0 : 1 ),
                            scrollbar_left ? 0 : getmaxx( win ) - 1, BORDER_COLOR, true );
        }

        if( !without_border ) {
            draw_custom_border( win, buffer.str().empty() );
            wrefresh( win );
        }

        if( without_getch ) {
            break;
        }

        // TODO: use input context
        result = inp_mngr.get_input_event();
        const int ch = static_cast<int>( result.get_first_input() );
        if( handle_scrolling && ch == KEY_PPAGE ) {
            selected--;
            werase( win );
        } else if( handle_scrolling && ch == KEY_NPAGE ) {
            selected++;
            werase( win );
        } else if( selected > 0 && ( ch == '\n' || ch == KEY_RIGHT ) ) {
            result = input_event( static_cast<long>( '\n' ), CATA_INPUT_KEYBOARD );
            break;
        } else if( selected == KEY_LEFT ) {
            result = input_event( static_cast<long>( ' ' ), CATA_INPUT_KEYBOARD );
            break;
        } else {
            break;
        }
    }

    return result;
}

char rand_char()
{
    switch( rng( 0, 9 ) ) {
        case 0:
            return '|';
        case 1:
            return '-';
        case 2:
            return '#';
        case 3:
            return '?';
        case 4:
            return '&';
        case 5:
            return '.';
        case 6:
            return '%';
        case 7:
            return '{';
        case 8:
            return '*';
        case 9:
            return '^';
    }
    return '?';
}

// this translates symbol y, u, n, b to NW, NE, SE, SW lines correspondingly
// h, j, c to horizontal, vertical, cross correspondingly
long special_symbol( long sym )
{
    switch( sym ) {
        case 'j':
            return LINE_XOXO;
        case 'h':
            return LINE_OXOX;
        case 'c':
            return LINE_XXXX;
        case 'y':
            return LINE_OXXO;
        case 'u':
            return LINE_OOXX;
        case 'n':
            return LINE_XOOX;
        case 'b':
            return LINE_XXOO;
        default:
            return sym;
    }
}

template<typename Prep>
std::string trim( const std::string &s, Prep prep )
{
    auto wsfront = std::find_if_not( s.begin(), s.end(), [&prep]( int c ) {
        return prep( c );
    } );
    return std::string( wsfront, std::find_if_not( s.rbegin(),
    std::string::const_reverse_iterator( wsfront ), [&prep]( int c ) {
        return prep( c );
    } ).base() );
}

std::string trim( const std::string &s )
{
    return trim( s, []( int c ) {
        return std::isspace( c );
    } );
}

std::string trim_punctuation_marks( const std::string &s )
{
    return trim( s, []( int c ) {
        return std::ispunct( c );
    } );
}

typedef std::string::value_type char_t;
std::string to_upper_case( const std::string &s )
{
    std::string res;
    std::transform( s.begin(), s.end(), std::back_inserter( res ), []( char_t ch ) {
        return std::use_facet<std::ctype<char_t>>( std::locale() ).toupper( ch );
    } );
    return res;
}

// find the position of each non-printing tag in a string
std::vector<size_t> get_tag_positions( const std::string &s )
{
    std::vector<size_t> ret;
    size_t pos = s.find( "<color_", 0, 7 );
    while( pos != std::string::npos ) {
        ret.push_back( pos );
        pos = s.find( "<color_", pos + 1, 7 );
    }
    pos = s.find( "</color>", 0, 8 );
    while( pos != std::string::npos ) {
        ret.push_back( pos );
        pos = s.find( "</color>", pos + 1, 8 );
    }
    std::sort( ret.begin(), ret.end() );
    return ret;
}

// utf-8 version
std::string word_rewrap( const std::string &in, int width, const uint32_t split )
{
    std::ostringstream o;

    // find non-printing tags
    std::vector<size_t> tag_positions = get_tag_positions( in );

    int lastwb  = 0; //last word break
    int lastout = 0;
    const char *instr = in.c_str();
    bool skipping_tag = false;
    bool just_wrapped = false;

    for( int j = 0, x = 0; j < static_cast<int>( in.size() ); ) {
        const char *ins = instr + j;
        int len = ANY_LENGTH;
        uint32_t uc = UTF8_getch( &ins, &len );

        if( uc == '<' ) { // maybe skip non-printing tag
            std::vector<size_t>::iterator it;
            for( it = tag_positions.begin(); it != tag_positions.end(); ++it ) {
                if( static_cast<int>( *it ) == j ) {
                    skipping_tag = true;
                    break;
                }
            }
        }

        const int old_j = j;
        j += ANY_LENGTH - len;

        if( skipping_tag ) {
            if( uc == '>' ) {
                skipping_tag = false;
            }
            continue;
        }

        if( just_wrapped && uc == ' ' ) { // ignore spaces after wrapping
            lastwb = lastout = j;
            continue;
        }

        x += mk_wcwidth( uc );

        if( uc == split || uc >= 0x2E80 ) { // param split (default ' ') or CJK characters
            if( x <= width ) {
                lastwb = j; // break after character
            } else {
                lastwb = old_j; // break before character
            }
        }

        if( x > width ) {
            if( lastwb == lastout ) {
                lastwb = old_j;
            }
            // old_j may equal to lastout, this checks it and ensures there's at least one character in the line.
            if( lastwb == lastout ) {
                lastwb = j;
            }
            for( int k = lastout; k < lastwb; k++ ) {
                o << in[k];
            }
            o << '\n';
            x = 0;
            lastout = j = lastwb;
            just_wrapped = true;
        } else {
            just_wrapped = false;
        }
    }
    for( int k = lastout; k < static_cast<int>( in.size() ); k++ ) {
        o << in[k];
    }

    return o.str();
}

void draw_tab( const catacurses::window &w, int iOffsetX, const std::string &sText, bool bSelected )
{
    int iOffsetXRight = iOffsetX + utf8_width( sText ) + 1;

    mvwputch( w, 0, iOffsetX,      c_light_gray, LINE_OXXO ); // |^
    mvwputch( w, 0, iOffsetXRight, c_light_gray, LINE_OOXX ); // ^|
    mvwputch( w, 1, iOffsetX,      c_light_gray, LINE_XOXO ); // |
    mvwputch( w, 1, iOffsetXRight, c_light_gray, LINE_XOXO ); // |

    mvwprintz( w, 1, iOffsetX + 1, ( bSelected ) ? h_light_gray : c_light_gray, sText );

    for( int i = iOffsetX + 1; i < iOffsetXRight; i++ ) {
        mvwputch( w, 0, i, c_light_gray, LINE_OXOX );  // -
    }

    if( bSelected ) {
        mvwputch( w, 1, iOffsetX - 1,      h_light_gray, '<' );
        mvwputch( w, 1, iOffsetXRight + 1, h_light_gray, '>' );

        for( int i = iOffsetX + 1; i < iOffsetXRight; i++ ) {
            mvwputch( w, 2, i, c_black, ' ' );
        }

        mvwputch( w, 2, iOffsetX,      c_light_gray, LINE_XOOX ); // _|
        mvwputch( w, 2, iOffsetXRight, c_light_gray, LINE_XXOO ); // |_

    } else {
        mvwputch( w, 2, iOffsetX,      c_light_gray, LINE_XXOX ); // _|_
        mvwputch( w, 2, iOffsetXRight, c_light_gray, LINE_XXOX ); // _|_
    }
}

void draw_subtab( const catacurses::window &w, int iOffsetX, const std::string &sText,
                  bool bSelected,
                  bool bDecorate, bool bDisabled )
{
    int iOffsetXRight = iOffsetX + utf8_width( sText ) + 1;

    if( ! bDisabled ) {
        mvwprintz( w, 0, iOffsetX + 1, ( bSelected ) ? h_light_gray : c_light_gray, sText );
    } else {
        mvwprintz( w, 0, iOffsetX + 1, ( bSelected ) ? h_dark_gray : c_dark_gray, sText );
    }

    if( bSelected ) {
        if( ! bDisabled ) {
            mvwputch( w, 0, iOffsetX - bDecorate,      h_light_gray, '<' );
            mvwputch( w, 0, iOffsetXRight + bDecorate, h_light_gray, '>' );
        } else {
            mvwputch( w, 0, iOffsetX - bDecorate,      h_dark_gray, '<' );
            mvwputch( w, 0, iOffsetXRight + bDecorate, h_dark_gray, '>' );
        }

        for( int i = iOffsetX + 1; bDecorate && i < iOffsetXRight; i++ ) {
            mvwputch( w, 1, i, c_black, ' ' );
        }
    }
}

/**
 * Draw a scrollbar (Legacy function, use class scrollbar instead!)
 * @param window Pointer of window to draw on
 * @param iCurrentLine The starting line or currently selected line out of the iNumLines lines
 * @param iContentHeight Height of the scrollbar
 * @param iNumLines Total number of lines
 * @param iOffsetY Y drawing offset
 * @param iOffsetX X drawing offset
 * @param bar_color Default line color
 * @param bDoNotScrollToEnd True if the last (iContentHeight-1) lines cannot be a start position or be selected
 *   If false, iCurrentLine can be from 0 to iNumLines - 1.
 *   If true, iCurrentLine can be at most iNumLines - iContentHeight.
 **/
void draw_scrollbar( const catacurses::window &window, const int iCurrentLine,
                     const int iContentHeight, const int iNumLines, const int iOffsetY, const int iOffsetX,
                     nc_color bar_color, const bool bDoNotScrollToEnd )
{
    scrollbar()
    .offset_x( iOffsetX )
    .offset_y( iOffsetY )
    .content_size( iNumLines )
    .viewport_pos( iCurrentLine )
    .viewport_size( iContentHeight )
    .slot_color( bar_color )
    .scroll_to_last( !bDoNotScrollToEnd )
    .apply( window );
}

scrollbar::scrollbar()
    : offset_x_v( 0 ), offset_y_v( 0 ), content_size_v( 0 ),
      viewport_pos_v( 0 ), viewport_size_v( 0 ),
      border_color_v( BORDER_COLOR ), arrow_color_v( c_light_green ),
      slot_color_v( c_white ), bar_color_v( c_cyan_cyan ), scroll_to_last_v( false )
{
}

scrollbar &scrollbar::offset_x( int offx )
{
    offset_x_v = offx;
    return *this;
}

scrollbar &scrollbar::offset_y( int offy )
{
    offset_y_v = offy;
    return *this;
}

scrollbar &scrollbar::content_size( int csize )
{
    content_size_v = csize;
    return *this;
}

scrollbar &scrollbar::viewport_pos( int vpos )
{
    viewport_pos_v = vpos;
    return *this;
}

scrollbar &scrollbar::viewport_size( int vsize )
{
    viewport_size_v = vsize;
    return *this;
}

scrollbar &scrollbar::border_color( nc_color border_c )
{
    border_color_v = border_c;
    return *this;
}

scrollbar &scrollbar::arrow_color( nc_color arrow_c )
{
    arrow_color_v = arrow_c;
    return *this;
}

scrollbar &scrollbar::slot_color( nc_color slot_c )
{
    slot_color_v = slot_c;
    return *this;
}

scrollbar &scrollbar::bar_color( nc_color bar_c )
{
    bar_color_v = bar_c;
    return *this;
}

scrollbar &scrollbar::scroll_to_last( bool scr2last )
{
    scroll_to_last_v = scr2last;
    return *this;
}

void scrollbar::apply( const catacurses::window &window )
{
    if( viewport_size_v >= content_size_v || content_size_v <= 0 ) {
        // scrollbar not needed, fill output area with borders
        for( int i = offset_y_v; i < offset_y_v + viewport_size_v; ++i ) {
            mvwputch( window, i, offset_x_v, border_color_v, LINE_XOXO );
        }
    } else {
        mvwputch( window, offset_y_v, offset_x_v, arrow_color_v, '^' );
        mvwputch( window, offset_y_v + viewport_size_v - 1, offset_x_v, arrow_color_v, 'v' );

        int slot_size = viewport_size_v - 2;
        int bar_size = std::max( 2, slot_size * viewport_size_v / content_size_v );
        int scrollable_size = scroll_to_last_v ? content_size_v : content_size_v - viewport_size_v + 1;

        int bar_start, bar_end;
        if( viewport_pos_v == 0 ) {
            bar_start = 0;
        } else if( scrollable_size > 2 ) {
            bar_start = ( slot_size - 1 - bar_size ) * ( viewport_pos_v - 1 ) / ( scrollable_size - 2 ) + 1;
        } else {
            bar_start = slot_size - bar_size;
        }
        bar_end = bar_start + bar_size;

        for( int i = 0; i < slot_size; ++i ) {
            if( i >= bar_start && i < bar_end ) {
                mvwputch( window, offset_y_v + 1 + i, offset_x_v, bar_color_v, LINE_XOXO );
            } else {
                mvwputch( window, offset_y_v + 1 + i, offset_x_v, slot_color_v, LINE_XOXO );
            }
        }
    }
}

void calcStartPos( int &iStartPos, const int iCurrentLine, const int iContentHeight,
                   const int iNumEntries )
{
    if( iNumEntries <= iContentHeight ) {
        iStartPos = 0;
    } else if( get_option<bool>( "MENU_SCROLL" ) ) {
        iStartPos = iCurrentLine - ( iContentHeight - 1 ) / 2;
        if( iStartPos < 0 ) {
            iStartPos = 0;
        } else if( iStartPos + iContentHeight > iNumEntries ) {
            iStartPos = iNumEntries - iContentHeight;
        }
    } else {
        if( iCurrentLine < iStartPos ) {
            iStartPos = iCurrentLine;
        } else if( iCurrentLine >= iStartPos + iContentHeight ) {
            iStartPos = 1 + iCurrentLine - iContentHeight;
        }
    }
}

catacurses::window w_hit_animation;
void hit_animation( int iX, int iY, nc_color cColor, const std::string &cTile )
{
    /*
    chtype chtOld = mvwinch(w, iY + VIEW_OFFSET_Y, iX + VIEW_OFFSET_X);
    mvwputch(w, iY + VIEW_OFFSET_Y, iX + VIEW_OFFSET_X, cColor, cTile);
    */

    catacurses::window w_hit = catacurses::newwin( 1, 1, iY + VIEW_OFFSET_Y, iX + VIEW_OFFSET_X );
    if( !w_hit ) {
        return; //we passed in negative values (semi-expected), so let's not segfault
    }
    w_hit_animation = w_hit;

    mvwprintz( w_hit, 0, 0, cColor, cTile );
    wrefresh( w_hit );

    inp_mngr.set_timeout( get_option<int>( "ANIMATION_DELAY" ) );
    // Skip input (if any), because holding down a key with nanosleep can get yourself killed
    inp_mngr.get_input_event();
    inp_mngr.reset_timeout();
}

#if defined(_MSC_VER)
std::string cata::string_formatter::raw_string_format( char const *const format, ... )
{
    va_list args;
    va_start( args, format );

    va_list args_copy;
    va_copy( args_copy, args );
    int const result = _vscprintf_p( format, args_copy );
    va_end( args_copy );
    if( result == -1 ) {
        throw std::runtime_error( "Bad format string for printf: \"" + std::string( format ) + "\"" );
    }

    std::string buffer( result, '\0' );
    _vsprintf_p( &buffer[0], result + 1, format, args ); //+1 for string's null
    va_end( args );

    return buffer;
}
#else

// Cygwin has limitations which prevents
// from using more than 9 positional arguments.
// This functions works around it in two ways:
//
// First if all positional arguments are in "natural" order
// (i.e. like %1$d %2$d %3$d),
// then their positions is stripped away and string
// formatted without positions.
//
// Otherwise only 9 arguments are passed to vsnprintf
//
std::string rewrite_vsnprintf( const char *msg )
{
    bool contains_positional = false;
    const char *orig_msg = msg;
    const char *formats = "diouxXeEfFgGaAcsCSpnm";

    std::ostringstream rewritten_msg;
    std::ostringstream rewritten_msg_optimised;
    const char *ptr = nullptr;
    int next_positional_arg = 1;
    while( true ) {

        // First find next position where argument might be used
        ptr = strchr( msg, '%' );
        if( ! ptr ) {
            rewritten_msg << msg;
            rewritten_msg_optimised << msg;
            break;
        }

        // Write portion of the string that was before %
        rewritten_msg << std::string( msg, ptr );
        rewritten_msg_optimised << std::string( msg, ptr );

        const char *arg_start = ptr;

        ptr++;

        // If it simply '%%', then no processing needed
        if( *ptr == '%' ) {
            rewritten_msg << "%%";
            rewritten_msg_optimised << "%%";
            msg = ptr + 1;
            continue;
        }

        // Parse possible number of positional argument
        int positional_arg = 0;
        while( isdigit( *ptr ) ) {
            positional_arg = positional_arg * 10 + *ptr - '0';
            ptr++;
        }

        // If '$' ever follows a numeral, the string has a positional arg
        if( *ptr == '$' ) {
            contains_positional = true;
        }

        // Check if it's expected argument
        if( *ptr == '$' && positional_arg == next_positional_arg ) {
            next_positional_arg++;
        } else {
            next_positional_arg = -1;
        }

        // Now find where it ends
        const char *end = strpbrk( ptr, formats );
        if( ! end ) {
            // Format string error. Just bail.
            return orig_msg;
        }

        // write entire argument to rewritten_msg
        if( positional_arg < 10 ) {
            std::string argument( arg_start, end + 1 );
            rewritten_msg << argument;
        } else {
            rewritten_msg << "<formatting error>";
        }

        // write argument without position to rewritten_msg_optimised
        if( next_positional_arg > 0 ) {
            std::string argument( ptr + 1, end + 1 );
            rewritten_msg_optimised << '%' << argument;
        }

        msg = end + 1;
    }

    if( !contains_positional ) {
        return orig_msg;
    }

    if( next_positional_arg > 0 ) {
        // If all positioned arguments were in order (%1$d %2$d) then we simply
        // strip arguments
        return rewritten_msg_optimised.str();
    }

    return rewritten_msg.str();
}

std::string cata::string_formatter::raw_string_format( char const *format, ... )
{
    va_list args;
    va_start( args, format );

    errno = 0; // Clear errno before trying
    std::vector<char> buffer( 1024, '\0' );

#if (defined __CYGWIN__)
    std::string rewritten_format = rewrite_vsnprintf( format );
    format = rewritten_format.c_str();
#endif

    for( ;; ) {
        size_t const buffer_size = buffer.size();

        va_list args_copy;
        va_copy( args_copy, args );
        int const result = vsnprintf( &buffer[0], buffer_size, format, args_copy );
        va_end( args_copy );

        // No error, and the buffer is big enough; we're done.
        if( result >= 0 && static_cast<size_t>( result ) < buffer_size ) {
            break;
        }

        // Standards conformant versions return -1 on error only.
        // Some non-standard versions return -1 to indicate a bigger buffer is needed.
        // Some of the latter set errno to ERANGE at the same time.
        if( result < 0 && errno && errno != ERANGE ) {
            throw std::runtime_error( "Bad format string for printf: \"" + std::string( format ) + "\"" );
        }

        // Looks like we need to grow... bigger, definitely bigger.
        buffer.resize( buffer_size * 2 );
    }

    va_end( args );
    return std::string( &buffer[0] );
}
#endif

void replace_name_tags( std::string &input )
{
    // these need to replace each tag with a new randomly generated name
    while( input.find( "<full_name>" ) != std::string::npos ) {
        replace_substring( input, "<full_name>", Name::get( nameIsFullName ),
                           false );
    }
    while( input.find( "<family_name>" ) != std::string::npos ) {
        replace_substring( input, "<family_name>", Name::get( nameIsFamilyName ),
                           false );
    }
    while( input.find( "<given_name>" ) != std::string::npos ) {
        replace_substring( input, "<given_name>", Name::get( nameIsGivenName ),
                           false );
    }
}

void replace_city_tag( std::string &input, const std::string &name )
{
    replace_substring( input, "<city>", name, true );
}

void replace_substring( std::string &input, const std::string &substring,
                        const std::string &replacement, bool all )
{
    if( all ) {
        while( input.find( substring ) != std::string::npos ) {
            replace_substring( input, substring, replacement, false );
        }
    } else {
        size_t len = substring.length();
        size_t offset = input.find( substring );
        input.replace( offset, len, replacement );
    }
}

//wrap if for i18n
std::string &capitalize_letter( std::string &str, size_t n )
{
    char c = str[n];
    if( str.length() > 0 && c >= 'a' && c <= 'z' ) {
        c += 'A' - 'a';
        str[n] = c;
    }

    return str;
}

//remove prefix of a strng, between c1 and c2, ie, "<prefix>remove it"
std::string rm_prefix( std::string str, char c1, char c2 )
{
    if( !str.empty() && str[0] == c1 ) {
        size_t pos = str.find_first_of( c2 );
        if( pos != std::string::npos ) {
            str = str.substr( pos + 1 );
        }
    }
    return str;
}

// draw a menu-item-like string with highlighted shortcut character
// Example: <w>ield, m<o>ve
// returns: output length (in console cells)
size_t shortcut_print( const catacurses::window &w, int y, int x, nc_color text_color,
                       nc_color shortcut_color, const std::string &fmt )
{
    wmove( w, y, x );
    return shortcut_print( w, text_color, shortcut_color, fmt );
}

//same as above, from current position
size_t shortcut_print( const catacurses::window &w, nc_color text_color, nc_color shortcut_color,
                       const std::string &fmt )
{
    std::string text = shortcut_text( shortcut_color, fmt );
    print_colored_text( w, -1, -1, text_color, text_color, text );

    return utf8_width( remove_color_tags( text ) );
}

//generate colorcoded shortcut text
std::string shortcut_text( nc_color shortcut_color, const std::string &fmt )
{
    size_t pos = fmt.find_first_of( '<' );
    size_t pos_end = fmt.find_first_of( '>' );
    if( pos_end != std::string::npos && pos < pos_end ) {
        size_t sep = std::min( fmt.find_first_of( '|', pos ), pos_end );
        std::string prestring = fmt.substr( 0, pos );
        std::string poststring = fmt.substr( pos_end + 1, std::string::npos );
        std::string shortcut = fmt.substr( pos + 1, sep - pos - 1 );

        return string_format( "%s<color_%s>%s</color>%s", prestring,
                              string_from_color( shortcut_color ).c_str(),
                              shortcut, poststring );
    }

    // no shortcut?
    return fmt;
}

std::pair<std::string, nc_color> const &
get_hp_bar( const int cur_hp, const int max_hp, const bool is_mon )
{
    using pair_t = std::pair<std::string, nc_color>;
    static std::array<pair_t, 12> const strings {
        {
            //~ creature health bars
            pair_t { R"(|||||)", c_green },
            pair_t { R"(||||\)", c_green },
            pair_t { R"(||||)",  c_light_green },
            pair_t { R"(|||\)",  c_light_green },
            pair_t { R"(|||)",   c_yellow },
            pair_t { R"(||\)",   c_yellow },
            pair_t { R"(||)",    c_light_red },
            pair_t { R"(|\)",    c_light_red },
            pair_t { R"(|)",     c_red },
            pair_t { R"(\)",     c_red },
            pair_t { R"(:)",     c_red },
            pair_t { R"(-----)", c_light_gray },
        }
    };

    double const ratio = static_cast<double>( cur_hp ) / ( max_hp ? max_hp : 1 );
    return ( ratio >= 1.0 )            ? strings[0]  :
           ( ratio >= 0.9 && !is_mon ) ? strings[1]  :
           ( ratio >= 0.8 )            ? strings[2]  :
           ( ratio >= 0.7 && !is_mon ) ? strings[3]  :
           ( ratio >= 0.6 )            ? strings[4]  :
           ( ratio >= 0.5 && !is_mon ) ? strings[5]  :
           ( ratio >= 0.4 )            ? strings[6]  :
           ( ratio >= 0.3 && !is_mon ) ? strings[7]  :
           ( ratio >= 0.2 )            ? strings[8]  :
           ( ratio >= 0.1 && !is_mon ) ? strings[9]  :
           ( ratio >  0.0 )            ? strings[10] : strings[11];
}

std::pair<std::string, nc_color> get_light_level( const float light )
{
    using pair_t = std::pair<std::string, nc_color>;
    static std::array<pair_t, 6> const strings {
        {
            pair_t {translate_marker( "unknown" ), c_pink},
            pair_t {translate_marker( "bright" ), c_yellow},
            pair_t {translate_marker( "cloudy" ), c_white},
            pair_t {translate_marker( "shady" ), c_light_gray},
            pair_t {translate_marker( "dark" ), c_dark_gray},
            pair_t {translate_marker( "very dark" ), c_black_white}
        }
    };
    // Avoid magic number
    static const int maximum_light_level = static_cast< int >( strings.size() ) - 1;
    const int light_level = clamp( static_cast< int >( ceil( light ) ), 0, maximum_light_level );
    const size_t array_index = static_cast< size_t >( light_level );
    return pair_t{ _( strings[array_index].first.c_str() ), strings[array_index].second };
}

std::string get_labeled_bar( const double val, const int width, const std::string &label, char c )
{
    const std::array<std::pair<double, char>, 1> ratings =
    {{ std::make_pair( 1.0, c ) }};
    return get_labeled_bar( val, width, label, ratings.begin(), ratings.end() );
}

/**
 * Display data in table, each cell contains one entry from the
 * data vector. Allows vertical scrolling if the data does not fit.
 * Data is displayed using fold_and_print_from, which allows coloring!
 * @param columns Number of columns, can be 1. Make sure each entry
 * of the data vector fits into one cell.
 * @param title The title text, displayed on top.
 * @param w The window to draw this in, the whole widow is used.
 * @param data Text data to fill.
 */
void display_table( const catacurses::window &w, const std::string &title, int columns,
                    const std::vector<std::string> &data )
{
    const int width = getmaxx( w ) - 2; // -2 for border
    const int rows = getmaxy( w ) - 2 - 1; // -2 for border, -1 for title
    const int col_width = width / columns;
    int offset = 0;

#ifdef __ANDROID__
    // no bindings, but give it its own input context so stale buttons don't hang around.
    input_context ctxt( "DISPLAY_TABLE" );
#endif
    for( ;; ) {
        werase( w );
        draw_border( w, BORDER_COLOR, title, c_white );
        for( int i = 0; i < rows * columns; i++ ) {
            if( i + offset * columns >= static_cast<int>( data.size() ) ) {
                break;
            }
            const int x = 2 + ( i % columns ) * col_width;
            const int y = ( i / columns ) + 2;
            fold_and_print_from( w, y, x, col_width, 0, c_white, data[i + offset * columns] );
        }
        draw_scrollbar( w, offset, rows, ( data.size() + columns - 1 ) / columns, 2, 0 );
        wrefresh( w );
        // TODO: use input context
        int ch = inp_mngr.get_input_event().get_first_input();
        if( ch == KEY_DOWN && ( ( offset + 1 ) * columns ) < static_cast<int>( data.size() ) ) {
            offset++;
        } else if( ch == KEY_UP && offset > 0 ) {
            offset--;
        } else if( ch == ' ' || ch == '\n' || ch == KEY_ESCAPE ) {
            break;
        }
    }
}

scrollingcombattext::cSCT::cSCT( const int p_iPosX, const int p_iPosY, const direction p_oDir,
                                 const std::string &p_sText, const game_message_type p_gmt,
                                 const std::string &p_sText2, const game_message_type p_gmt2,
                                 const std::string &p_sType )
{
    iPosX = p_iPosX;
    iPosY = p_iPosY;
    sType = p_sType;
    oDir = p_oDir;

    // translate from player relative to screen relative direction
    iso_mode = false;
#ifdef TILES
    iso_mode = tile_iso && use_tiles;
#endif
    oUp = iso_mode ? NORTHEAST : NORTH;
    oUpRight = iso_mode ? EAST : NORTHEAST;
    oRight = iso_mode ? SOUTHEAST : EAST;
    oDownRight = iso_mode ? SOUTH : SOUTHEAST;
    oDown = iso_mode ? SOUTHWEST : SOUTH;
    oDownLeft = iso_mode ? WEST : SOUTHWEST;
    oLeft = iso_mode ? NORTHWEST : WEST;
    oUpLeft = iso_mode ? NORTH : NORTHWEST;

    point pairDirXY = direction_XY( oDir );

    iDirX = pairDirXY.x;
    iDirY = pairDirXY.y;

    if( iDirX == 0 && iDirY == 0 ) {
        // This would cause infinite loop otherwise
        oDir = WEST;
        iDirX = -1;
    }

    iStep = 0;
    iStepOffset = 0;

    sText = p_sText;
    gmt = p_gmt;

    sText2 = p_sText2;
    gmt2 = p_gmt2;

}

void scrollingcombattext::add( const int p_iPosX, const int p_iPosY, direction p_oDir,
                               const std::string &p_sText, const game_message_type p_gmt,
                               const std::string &p_sText2, const game_message_type p_gmt2,
                               const std::string &p_sType )
{
    if( get_option<bool>( "ANIMATION_SCT" ) ) {

        int iCurStep = 0;

        bool tiled = false;
        bool iso_mode = false;
#ifdef TILES
        tiled = use_tiles;
        iso_mode = tile_iso && use_tiles;
#endif

        if( p_sType == "hp" ) {
            //Remove old HP bar
            removeCreatureHP();

            if( p_oDir == WEST || p_oDir == NORTHWEST || p_oDir == ( iso_mode ? NORTH : SOUTHWEST ) ) {
                p_oDir = ( iso_mode ? NORTHWEST : WEST );
            } else {
                p_oDir = ( iso_mode ? SOUTHEAST : EAST );
            }

        } else {
            //reserve Left/Right for creature hp display
            if( p_oDir == ( iso_mode ? SOUTHEAST : EAST ) ) {
                p_oDir = ( one_in( 2 ) ) ? ( iso_mode ? EAST : NORTHEAST ) : ( iso_mode ? SOUTH : SOUTHEAST );

            } else if( p_oDir == ( iso_mode ? NORTHWEST : WEST ) ) {
                p_oDir = ( one_in( 2 ) ) ? ( iso_mode ? NORTH : NORTHWEST ) : ( iso_mode ? WEST : SOUTHWEST );
            }
        }

        // in tiles, SCT that scroll downwards are inserted at the beginning of the vector to prevent
        // oversize ASCII tiles overdrawing messages below them.
        if( tiled && ( p_oDir == SOUTHWEST || p_oDir == SOUTH ||
                       p_oDir == ( iso_mode ? WEST : SOUTHEAST ) ) ) {

            //Message offset: multiple impacts in the same direction in short order overriding prior messages (mostly turrets)
            for( std::vector<cSCT>::iterator iter = vSCT.begin(); iter != vSCT.end(); ++iter ) {
                if( iter->getDirecton() == p_oDir && ( iter->getStep() + iter->getStepOffset() ) == iCurStep ) {
                    ++iCurStep;
                    iter->advanceStepOffset();
                }
            }
            vSCT.insert( vSCT.begin(), cSCT( p_iPosX, p_iPosY, p_oDir, p_sText, p_gmt, p_sText2, p_gmt2,
                                             p_sType ) );

        } else {
            //Message offset: this time in reverse.
            for( std::vector<cSCT>::reverse_iterator iter = vSCT.rbegin(); iter != vSCT.rend(); ++iter ) {
                if( iter->getDirecton() == p_oDir && ( iter->getStep() + iter->getStepOffset() ) == iCurStep ) {
                    ++iCurStep;
                    iter->advanceStepOffset();
                }
            }
            vSCT.push_back( cSCT( p_iPosX, p_iPosY, p_oDir, p_sText, p_gmt, p_sText2, p_gmt2, p_sType ) );
        }

    }
}

std::string scrollingcombattext::cSCT::getText( std::string const &type ) const
{
    if( !sText2.empty() ) {
        if( oDir == oUpLeft || oDir == oDownLeft || oDir == oLeft ) {
            if( type == "first" ) {
                return sText2 + " ";

            } else if( type == "full" ) {
                return sText2 + " " + sText;
            }
        } else {
            if( type == "second" ) {
                return " " + sText2;
            } else if( type == "full" ) {
                return sText + " " + sText2;
            }
        }
    } else if( type == "second" ) {
        return {};
    }

    return sText;
}

game_message_type scrollingcombattext::cSCT::getMsgType( std::string const &type ) const
{
    if( !sText2.empty() ) {
        if( oDir == oUpLeft || oDir == oDownLeft || oDir == oLeft ) {
            if( type == "first" ) {
                return gmt2;
            }
        } else {
            if( type == "second" ) {
                return gmt2;
            }
        }
    }

    return gmt;
}

int scrollingcombattext::cSCT::getPosX() const
{
    if( getStep() > 0 ) {
        int iDirOffset = ( oDir == oRight ) ? 1 : ( ( oDir == oLeft ) ? -1 : 0 );

        if( oDir == oUp || oDir == oDown ) {

            if( iso_mode ) {
                iDirOffset = ( oDir == oUp ) ? 1 : -1;
            }

            //Center text
            iDirOffset -= ( getText().length() / 2 );

        } else if( oDir == oLeft || oDir == oDownLeft || oDir == oUpLeft ) {
            //Right align text
            iDirOffset -= getText().length() - 1;
        }

        return iPosX + iDirOffset + ( iDirX * ( ( sType == "hp" ) ? ( getStepOffset() + 1 ) :
                                                ( getStepOffset() * ( iso_mode ? 2 : 1 ) + getStep() ) ) );
    }

    return 0;
}

int scrollingcombattext::cSCT::getPosY() const
{
    if( getStep() > 0 ) {
        int iDirOffset = ( oDir == oDown ) ? 1 : ( ( oDir == oUp ) ? -1 : 0 );

        if( iso_mode ) {
            if( oDir == oLeft || oDir == oRight ) {
                iDirOffset = ( oDir == oRight ) ? 1 : -1;
            }

            if( oDir == oUp || oDir == oDown ) {
                //Center text
                iDirOffset -= ( getText().length() / 2 );

            } else if( oDir == oLeft || oDir == oDownLeft || oDir == oUpLeft ) {
                //Right align text
                iDirOffset -= getText().length() - 1;
            }

        }

        return iPosY + iDirOffset + ( iDirY * ( ( iso_mode && sType == "hp" ) ? ( getStepOffset() + 1 ) :
                                                ( getStepOffset() * ( iso_mode ? 2 : 1 ) + getStep() ) ) );
    }

    return 0;
}

void scrollingcombattext::advanceAllSteps()
{
    std::vector<cSCT>::iterator iter = vSCT.begin();

    while( iter != vSCT.end() ) {
        if( iter->advanceStep() > this->iMaxSteps ) {
            iter = vSCT.erase( iter );
        } else {
            ++iter;
        }
    }
}

void scrollingcombattext::removeCreatureHP()
{
    //check for previous hp display and delete it
    for( std::vector<cSCT>::iterator iter = vSCT.begin(); iter != vSCT.end(); ++iter ) {
        if( iter->getType() == "hp" ) {
            vSCT.erase( iter );
            break;
        }
    }
}

nc_color msgtype_to_color( const game_message_type type, const bool bOldMsg )
{
    static std::map<game_message_type, std::pair<nc_color, nc_color>> const colors {
        {m_good,     {c_light_green, c_green}},
        {m_bad,      {c_light_red,   c_red}},
        {m_mixed,    {c_pink,    c_magenta}},
        {m_warning,  {c_yellow,  c_brown}},
        {m_info,     {c_light_blue,  c_blue}},
        {m_neutral,  {c_white,   c_light_gray}},
        {m_debug,    {c_white,   c_light_gray}},
        {m_headshot, {c_pink,    c_magenta}},
        {m_critical, {c_yellow,  c_brown}},
        {m_grazing,  {c_light_blue,  c_blue}}
    };

    auto const it = colors.find( type );
    if( it == std::end( colors ) ) {
        return bOldMsg ? c_light_gray : c_white;
    }

    return bOldMsg ? it->second.second : it->second.first;
}

/**
 * Match text containing wildcards (*)
 * @param text_in Text to check
 * @param pattern_in Pattern to check text_in against
 * Case insensitive search
 * Possible patterns:
 * *
 * wooD
 * wood*
 * *wood
 * Wood*aRrOW
 * wood*arrow*
 * *wood*arrow
 * *wood*hard* *x*y*z*arrow*
 **/
bool wildcard_match( const std::string &text_in, const std::string &pattern_in )
{
    std::string text = text_in;

    if( text.empty() ) {
        return false;
    } else if( text == "*" ) {
        return true;
    }

    int pos;
    std::vector<std::string> pattern = string_split( wildcard_trim_rule( pattern_in ), '*' );

    if( pattern.size() == 1 ) { // no * found
        return ( text.length() == pattern[0].length() && ci_find_substr( text, pattern[0] ) != -1 );
    }

    for( auto it = pattern.begin(); it != pattern.end(); ++it ) {
        if( it == pattern.begin() && *it != "" ) {
            if( text.length() < it->length() ||
                ci_find_substr( text.substr( 0, it->length() ), *it ) == -1 ) {
                return false;
            }

            text = text.substr( it->length(), text.length() - it->length() );
        } else if( it == pattern.end() - 1 && *it != "" ) {
            if( text.length() < it->length() ||
                ci_find_substr( text.substr( text.length() - it->length(),
                                             it->length() ), *it ) == -1 ) {
                return false;
            }
        } else {
            if( !( *it ).empty() ) {
                pos = ci_find_substr( text, *it );
                if( pos == -1 ) {
                    return false;
                }

                text = text.substr( pos + static_cast<int>( it->length() ),
                                    static_cast<int>( text.length() ) - pos );
            }
        }
    }

    return true;
}

std::string wildcard_trim_rule( const std::string &pattern_in )
{
    std::string pattern = pattern_in;
    size_t pos = pattern.find( "**" );

    //Remove all double ** in pattern
    while( pos != std::string::npos ) {
        pattern = pattern.substr( 0, pos ) + pattern.substr( pos + 1, pattern.length() - pos - 1 );
        pos = pattern.find( "**" );
    }

    return pattern;
}

std::vector<std::string> string_split( const std::string &text_in, char delim_in )
{
    std::vector<std::string> elems;

    if( text_in.empty() ) {
        return elems; // Well, that was easy.
    }

    std::stringstream ss( text_in );
    std::string item;
    while( std::getline( ss, item, delim_in ) ) {
        elems.push_back( item );
    }

    if( text_in[text_in.length() - 1] == delim_in ) {
        elems.push_back( "" );
    }

    return elems;
}

// find substring (case insensitive)
int ci_find_substr( const std::string &str1, const std::string &str2, const std::locale &loc )
{
    std::string::const_iterator it = std::search( str1.begin(), str1.end(), str2.begin(), str2.end(),
    [&]( const char str1_in, const char str2_in ) {
        return std::toupper( str1_in, loc ) == std::toupper( str2_in, loc );
    } );
    if( it != str1.end() ) {
        return it - str1.begin();
    } else {
        return -1;    // not found
    }
}

/**
* Convert, round up and format a volume.
*/
std::string format_volume( const units::volume &volume )
{
    return format_volume( volume, 0, NULL, NULL );
}

/**
* Convert, clamp, round up and format a volume,
* taking into account the specified width (0 for unlimited space),
* optionally returning a flag that indicate if the value was truncated to fit the width,
* optionally returning the formatted value as double.
*/
std::string format_volume( const units::volume &volume, int width, bool *out_truncated,
                           double *out_value )
{
    // convert and get the units preferred scale
    int scale = 0;
    double value = convert_volume( volume.value(), &scale );
    // clamp to the specified width
    if( width != 0 ) {
        value = clamp_to_width( value, std::abs( width ), scale, out_truncated );
    }
    // round up
    value = round_up( value, scale );
    if( out_value != nullptr ) {
        *out_value = value;
    }
    // format
    if( width < 0 ) {
        // left-justify the specified width
        return string_format( "%-*.*f", std::abs( width ), scale, value );
    } else if( width > 0 ) {
        // right-justify the specified width
        return string_format( "%*.*f", width, scale, value );
    } else {
        // no width
        return string_format( "%.*f", scale, value );
    }
}

// In non-SDL mode, width/height is just what's specified in the menu
#if !defined(TILES)
// We need to override these for Windows console resizing
#if !(defined _WIN32 || defined __WIN32__)
int get_terminal_width()
{
    int width = get_option<int>( "TERMINAL_X" );
    return width < FULL_SCREEN_WIDTH ? FULL_SCREEN_WIDTH : width;
}

int get_terminal_height()
{
    return get_option<int>( "TERMINAL_Y" );
}
#endif

bool is_draw_tiles_mode()
{
    return false;
}

void play_music( std::string )
{
}

void update_music_volume()
{
}

void refresh_display()
{
}
#endif

void mvwprintz( const catacurses::window &w, const int y, const int x, const nc_color &FG,
                const std::string &text )
{
    wattron( w, FG );
    mvwprintw( w, y, x, text );
    wattroff( w, FG );
}

void wprintz( const catacurses::window &w, const nc_color &FG, const std::string &text )
{
    wattron( w, FG );
    wprintw( w, text );
    wattroff( w, FG );
}
