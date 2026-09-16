import * as cdk from 'aws-cdk-lib';
import { Match, Template } from 'aws-cdk-lib/assertions';
import { FleetStack, partitionFleet, FleetEntry, isRealHostId } from '../lib/fleet';

const KEY_PAIR = 'test-key';
const REGION = 'us-east-1';

function synthStack(entries: FleetEntry[]): Template {
  const app = new cdk.App();
  const { primaryEntries } = partitionFleet(entries, REGION);
  const stack = new FleetStack(app, 'TestStack', {
    env: { region: REGION, account: '123456789012' },
    keyPairName: KEY_PAIR,
    amiId: 'ami-0123456789abcdef0',
    entries: primaryEntries,
    regionName: REGION,
  });
  return Template.fromStack(stack);
}

describe('tenancy', () => {
  test('defaults to shared (no Tenancy CFN property set)', () => {
    const template = synthStack([{ count: 1 }]);
    template.hasResourceProperties('AWS::EC2::Instance', {
      Tenancy: Match.absent(),
    });
  });

  test('tenancy "instance" sets CfnInstance.Tenancy to dedicated', () => {
    const template = synthStack([{ count: 1, tenancy: 'instance' }]);
    template.hasResourceProperties('AWS::EC2::Instance', {
      Tenancy: 'dedicated',
    });
  });

  test('tenancy "instance" is compatible with a cluster placement group', () => {
    expect(() => synthStack([
      { count: 2, tenancy: 'instance', pgType: 'cluster', pgName: 'cpg-a' },
    ])).not.toThrow();
  });

  test('tenancy "instance" rejected with a spread placement group', () => {
    expect(() => synthStack([
      { count: 1, tenancy: 'instance', pgType: 'spread' },
    ])).toThrow(/spread/i);
  });

  test('tenancy "host" is rejected with a spread placement group', () => {
    expect(() => synthStack([
      { count: 1, tenancy: 'host', pgType: 'spread' },
    ])).toThrow(/spread/i);
  });

  test('invalid tenancy value is rejected', () => {
    expect(() => synthStack([{ count: 1, tenancy: 'bogus' as unknown as FleetEntry['tenancy'] }]))
      .toThrow(/Invalid tenancy/i);
  });

  test('each instance in a mixed-tenancy scenario gets its own tenancy', () => {
    const template = synthStack([
      { count: 1, role: 'source', tenancy: 'shared' },
      { count: 1, role: 'destination', tenancy: 'instance' },
    ]);
    const instances = template.findResources('AWS::EC2::Instance');
    const tenancies = Object.values(instances).map((r: any) => r.Properties?.Tenancy);
    expect(tenancies.filter((t) => t === 'dedicated')).toHaveLength(1);
    expect(tenancies.filter((t) => t === undefined)).toHaveLength(1);
  });

  test('Tenancy tag reflects the resolved value (defaulting to "shared")', () => {
    const template = synthStack([{ count: 1 }]);
    template.hasResourceProperties('AWS::EC2::Instance', {
      Tags: Match.arrayWith([{ Key: 'Tenancy', Value: 'shared' }]),
    });
  });
});

describe('tenancy "host" (Dedicated Host)', () => {
  test('allocates a CfnHost and points the instance at it via affinity + hostId', () => {
    const template = synthStack([{ count: 1, tenancy: 'host', type: 'c7i.4xlarge', az: 'a' }]);
    template.resourceCountIs('AWS::EC2::Host', 1);
    template.hasResourceProperties('AWS::EC2::Host', {
      InstanceType: 'c7i.4xlarge',
      AvailabilityZone: `${REGION}a`,
      AutoPlacement: 'off',
    });
    const hosts = template.findResources('AWS::EC2::Host');
    const hostLogicalId = Object.keys(hosts)[0];
    template.hasResourceProperties('AWS::EC2::Instance', {
      Tenancy: 'host',
      Affinity: 'host',
      HostId: { 'Fn::GetAtt': [hostLogicalId, 'HostId'] },
    });
  });

  test('entries sharing (type, AZ) share ONE host; a different type/AZ gets its own', () => {
    const template = synthStack([
      { count: 2, tenancy: 'host', type: 'c7i.4xlarge', az: 'a' }, // same host
      { count: 1, tenancy: 'host', type: 'c7i.2xlarge', az: 'a' }, // different type -> new host
      { count: 1, tenancy: 'host', type: 'c7i.4xlarge', az: 'b' }, // different AZ -> new host
    ]);
    template.resourceCountIs('AWS::EC2::Host', 3);
    const instances = template.findResources('AWS::EC2::Instance');
    const hostIds = Object.values(instances).map((r: any) => JSON.stringify(r.Properties?.HostId));
    // The two same (type,AZ) instances reference the identical host logical id.
    const counts = new Map<string, number>();
    for (const h of hostIds) counts.set(h, (counts.get(h) ?? 0) + 1);
    expect([...counts.values()].sort()).toEqual([1, 1, 2]);
  });

  test('mixing "host" tenancy with a cluster placement group is allowed', () => {
    expect(() => synthStack([
      { count: 2, tenancy: 'host', pgType: 'cluster', pgName: 'cpg-a' },
    ])).not.toThrow();
  });

  test('hostId targets a pre-existing host without allocating a new CfnHost', () => {
    const template = synthStack([
      { count: 1, tenancy: 'host', hostId: 'h-0123456789abcdef0' },
    ]);
    template.resourceCountIs('AWS::EC2::Host', 0);
    template.hasResourceProperties('AWS::EC2::Instance', {
      Tenancy: 'host',
      Affinity: 'host',
      HostId: 'h-0123456789abcdef0',
    });
  });

  test('hostId without tenancy:"host" is rejected', () => {
    expect(() => synthStack([
      { count: 1, hostId: 'h-0123456789abcdef0' },
    ])).toThrow(/hostId .* requires tenancy/i);
  });

  test('hostId alias ("a", not a real "h-..." id) allocates ONE host shared by every row using it', () => {
    const template = synthStack([
      { count: 1, role: 'source', tenancy: 'host', hostId: 'a', type: 'c7i.4xlarge', az: 'a' },
      { count: 1, role: 'destination', tenancy: 'host', hostId: 'a', type: 'c7i.4xlarge', az: 'a' },
    ]);
    template.resourceCountIs('AWS::EC2::Host', 1);
    const hosts = template.findResources('AWS::EC2::Host');
    const hostLogicalId = Object.keys(hosts)[0];
    const instances = template.findResources('AWS::EC2::Instance');
    const hostIds = Object.values(instances).map((r: any) => JSON.stringify(r.Properties?.HostId));
    expect(hostIds).toEqual([
      JSON.stringify({ 'Fn::GetAtt': [hostLogicalId, 'HostId'] }),
      JSON.stringify({ 'Fn::GetAtt': [hostLogicalId, 'HostId'] }),
    ]);
  });

  test('a hostId alias is independent of the (type, AZ) auto-grouping key', () => {
    // Same (type, AZ) as an alias-less entry elsewhere should NOT collide
    // with the alias group - two distinct hosts.
    const template = synthStack([
      { count: 1, tenancy: 'host', type: 'c7i.4xlarge', az: 'a' },        // auto-grouped, no alias
      { count: 1, tenancy: 'host', hostId: 'a', type: 'c7i.4xlarge', az: 'a' }, // alias "a", same (type,az)
    ]);
    template.resourceCountIs('AWS::EC2::Host', 2);
  });

  test('a hostId alias used with inconsistent (type, AZ) across rows is rejected', () => {
    expect(() => synthStack([
      { count: 1, tenancy: 'host', hostId: 'a', type: 'c7i.4xlarge', az: 'a' },
      { count: 1, tenancy: 'host', hostId: 'a', type: 'c7i.2xlarge', az: 'a' }, // different type, same alias
    ])).toThrow(/inconsistent/i);
  });
});

describe('isRealHostId', () => {
  test('recognizes a real AWS host id', () => {
    expect(isRealHostId('h-0123456789abcdef0')).toBe(true);
    expect(isRealHostId('h-0ba1c2d3e4f56789a')).toBe(true);
  });

  test('treats anything else as an alias, not a real id', () => {
    expect(isRealHostId('a')).toBe(false);
    expect(isRealHostId('my-host')).toBe(false);
    expect(isRealHostId('h-tooshort')).toBe(false);
    expect(isRealHostId('h-0123456789ABCDEF0')).toBe(false); // uppercase hex not valid
    expect(isRealHostId('i-0123456789abcdef0')).toBe(false); // instance id, not host id
  });
});

describe('placement group validation (pre-existing behavior)', () => {
  test('cluster placement group spanning multiple AZs is rejected', () => {
    expect(() => synthStack([
      { count: 1, az: 'a', pgType: 'cluster', pgName: 'cpg-a' },
      { count: 1, az: 'b', pgType: 'cluster', pgName: 'cpg-a' },
    ])).toThrow(/same AZ/i);
  });

  test('spread placement group over 7 in one AZ is rejected', () => {
    expect(() => synthStack([{ count: 8, az: 'a', pgType: 'spread' }])).toThrow(/max 7/i);
  });
});
