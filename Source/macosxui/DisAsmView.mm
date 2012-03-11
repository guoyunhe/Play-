#import "DisAsmView.h"
#import "countof.h"
#import "lexical_cast_ex.h"
#import "Globals.h"
#import "ObjCCall.h"

using namespace std;

@implementation CDisAsmView

-(id)initWithFrame : (NSRect)frameRect
{
	m_context = NULL;
	m_selectionAddress = 0;
	m_font = [NSFont fontWithName:@"Courier" size:14];
	[m_font retain];
	m_textHeight = [m_font defaultLineHeightForFont];
	return [super initWithFrame:frameRect];
}

-(void)dealloc
{
	[m_font release];
	[super dealloc];
}

-(bool)acceptsFirstResponder
{
	return true;
}

-(bool)isFlipped
{
	return true;
}

-(void)gotoPc: (id)sender
{
	[self ensurePcVisible];
}

-(void)gotoAddress: (id)sender
{
	string currentAddress = "0x" + lexical_cast_hex<string>(m_viewAddress, 8);
	[m_inputBox setLabelText: @"Goto address:"];
	[m_inputBox setStringValue: [NSString stringWithCString: currentAddress.c_str()]];
	[m_inputBox doModal: [self window] callbackObj: self callback: @selector(gotoAddressDone:returnCode:)];
}

-(void)gotoAddressDone: (NSWindow*)sheet returnCode: (int)returnCode
{
	if(returnCode != YES) return;
	string addressString = [[m_inputBox stringValue] UTF8String];
	try
	{
		uint32 newAddress = lexical_cast_hex<string>(addressString);
		if((newAddress & 0x03) == 0)
		{
			m_viewAddress = newAddress;
			[self setNeedsDisplay: true];
		}
	}
	catch(...)
	{
		
	}
}

-(void)drawRect : (NSRect)rect
{
//	NSGraphicsContext* context = [NSGraphicsContext currentContext];	

	[[NSColor whiteColor] set];
	NSRectFill(rect);

	int marginX = 2;
	
	if(m_context != NULL && m_context->m_pArch != NULL)
	{
		uint32 address = m_viewAddress;
		int rowCount = (rect.size.height / m_textHeight) + 1;

		for(unsigned int row = 0; row < rowCount; row++)
		{
			int posY = m_textHeight * row;
			
			if(address == m_context->m_State.nPC)
			{
				[[NSColor blueColor] set];
				NSRectFill(NSMakeRect(0, posY, rect.size.width, m_textHeight));
			}
			else if(m_context->m_breakpoints.find(address) != m_context->m_breakpoints.end())
			{
				[[NSColor redColor] set];
				NSRectFill(NSMakeRect(0, posY, rect.size.width, m_textHeight));				
			}
			else if(address == m_selectionAddress)
			{
				[[NSColor selectedTextBackgroundColor] set];
				NSRectFill(NSMakeRect(0, posY, rect.size.width, m_textHeight));
			}
			
			char mnemonicText[256];
			char operandsText[256];
			string addressText = lexical_cast_hex<string>(address, 8);
			string functionText;
			
			uint32 opcode = m_context->m_pMemoryMap->GetWord(address);
			m_context->m_pArch->GetInstructionMnemonic(m_context, address, opcode, mnemonicText, countof(mnemonicText));
			m_context->m_pArch->GetInstructionOperands(m_context, address, opcode, operandsText, countof(operandsText));
			string opcodeText = lexical_cast_hex<string>(opcode, 8);
			uint32 effectiveAddress = m_context->m_pArch->GetInstructionEffectiveAddress(m_context, address, opcode);
			const char* functionTag = m_context->m_Functions.Find(address);
			const char* targetTag = m_context->m_Functions.Find(effectiveAddress);
			
			if(targetTag != NULL)
			{
				functionText = "-> " + string(targetTag);
			}
			else if(functionTag != NULL)
			{
				functionText = "@ " + string(functionTag);
			}

			NSString* mnemonicString = [NSString stringWithCString: mnemonicText];
			NSString* operandsString = [NSString stringWithCString: operandsText];
			NSString* addressString = [NSString stringWithCString: addressText.c_str()];
			NSString* opcodeString = [NSString stringWithCString: opcodeText.c_str()];
			NSString* functionString = [NSString stringWithCString: functionText.c_str()];

			NSMutableDictionary *textAttrs = [NSMutableDictionary dictionary];
			[textAttrs setObject:m_font forKey:NSFontAttributeName];
			[addressString drawAtPoint  : NSMakePoint(marginX +   0, posY) withAttributes: textAttrs];
			[opcodeString drawAtPoint   : NSMakePoint(marginX +  75, posY) withAttributes: textAttrs];
			[mnemonicString drawAtPoint : NSMakePoint(marginX + 150, posY) withAttributes: textAttrs];
			[operandsString drawAtPoint : NSMakePoint(marginX + 250, posY) withAttributes: textAttrs];
			[functionString drawAtPoint : NSMakePoint(marginX + 400, posY) withAttributes: textAttrs];
			
			address += 4;
		}
	}
}

-(void)setContext : (CMIPS*)context
{
	m_context = context;
	g_virtualMachine->OnMachineStateChange.connect(ObjCCall(self, "onMachineStateChange"));
	m_viewAddress = m_context->m_State.nPC;
	[self ensurePcVisible];
	[self setNeedsDisplay:true];
}

-(void)keyDown : (NSEvent*)event
{
	NSString* characters = [event characters];
	if([characters length] != 0)
	{
		unichar character = [characters characterAtIndex:0];
		switch(character)
		{
		case NSDownArrowFunctionKey:
			{
				[self onDownArrowKey];
				return;
			}
			break;
		case NSUpArrowFunctionKey:
			{
				[self onUpArrowKey];
				return;
			}
			break;
		case L'b':
			{
				m_context->ToggleBreakpoint(m_selectionAddress);
				[self setNeedsDisplay: true];
				return;
			}
			break;
		case L'f':
			{
				g_virtualMachine->StepEe();
				return;
			}
			break;
		}
	}
	return [super keyDown:event];
}

-(void)mouseDown : (NSEvent*)event
{
	NSPoint clickPoint = [self convertPoint:[event locationInWindow] fromView:[[self window] contentView]];
	m_selectionAddress = (static_cast<int>(clickPoint.y / m_textHeight) * 4) + m_viewAddress;
	[self setNeedsDisplay: true];
}

-(void)ensurePcVisible
{
	if([self isAddressInvisible: m_context->m_State.nPC])
	{
		m_viewAddress = m_context->m_State.nPC;
	}
	[self setNeedsDisplay:true];
}

-(uint32)maxViewAddress
{
	NSRect bounds = [self bounds];
	return (static_cast<int>(bounds.size.height / m_textHeight) * 4) + m_viewAddress;
}

-(void)onDownArrowKey
{
	m_selectionAddress += 4;
	if([self isAddressInvisible: m_selectionAddress])
	{
		uint32 viewCount = [self maxViewAddress] - m_viewAddress;
		m_viewAddress = m_selectionAddress - viewCount / 2;
	}
	[self setNeedsDisplay: true];
}

-(void)onUpArrowKey
{
	m_selectionAddress -= 4;
	if([self isAddressInvisible: m_selectionAddress])
	{
		uint32 viewCount = [self maxViewAddress] - m_viewAddress;
		m_viewAddress = m_selectionAddress - viewCount / 2;
	}
	[self setNeedsDisplay: true];
}

-(bool)isAddressInvisible: (uint32)address
{
	return static_cast<int32>(address) < static_cast<int32>(m_viewAddress) ||
		static_cast<int32>(address) > static_cast<int32>([self maxViewAddress]);
}

-(void)onMachineStateChange
{
	[self ensurePcVisible];
	m_selectionAddress = m_context->m_State.nPC;
	[self setNeedsDisplay:true];
}

@end
